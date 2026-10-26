// Copyright (c) 2026 Alex Coulombe. Licensed under the MIT License.

#include "BlueprintAutoLayout.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Comment.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "BPALMaterialTools.h"
#include "BPALLayeredLayout.h"
#include "BlueprintAutoLayoutSettings.h"
#if BPAL_MATERIAL_TOOLS
#include "Materials/MaterialExpressionNamedReroute.h"
#endif

int32 FBlueprintAutoLayout::LayoutMaterialGraph(UEdGraph* Graph, const TArray<UEdGraphNode*>* Selection, int32 StartX, int32 StartY)
{
	UMaterialGraph* MaterialGraph = CastChecked<UMaterialGraph>(Graph);
	NodeInfoMap.Empty();
	RootNodes.Empty();
	AllExecNodes.Empty();
	AllPureNodes.Empty();
	AllKnots.Empty();
	AllComments.Empty();
	PositionedPureNodes.Empty();
	CommentMembers.Empty();
	PendingLaneRoutes.Empty();
	LiveGraphPanel = nullptr;
	ResolveLiveGraphPanel(Graph);
	CaptureCommentMembership(Graph);
#if BPAL_MATERIAL_TOOLS
	// Preserve nested comment containment as well as expression membership.
	for (UEdGraphNode* Outer : Graph->Nodes)
	{
		if (!Outer || !IsComment(Outer)) continue;
		for (UEdGraphNode* Inner : Graph->Nodes)
		{
			if (!Inner || Inner == Outer || !IsComment(Inner)) continue;
			if (Inner->NodePosX >= Outer->NodePosX && Inner->NodePosY >= Outer->NodePosY &&
				Inner->NodePosX + Inner->NodeWidth <= Outer->NodePosX + Outer->NodeWidth &&
				Inner->NodePosY + Inner->NodeHeight <= Outer->NodePosY + Outer->NodeHeight &&
				(Inner->NodeWidth < Outer->NodeWidth || Inner->NodeHeight < Outer->NodeHeight))
				CommentMembers.FindOrAdd(Outer).AddUnique(Inner);
		}
	}
#endif

	// Only selected vertices enter the solver. Boundary links and unselected nodes stay put.
	// Material reroutes remain ordinary vertices, preserving their expressions and links.
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || (Selection && !Selection->Contains(Node))) continue;
		FLayoutNodeInfo* Info = GetOrCreateNodeInfo(Node);
		Info->bIsComment = IsComment(Node);
		CalculateNodeDimensions(Info);
		if (Info->bIsComment) AllComments.Add(Info);
	}

	// Materials have no execution roots. Solve included data-flow vertices directly.
	const bool bSavedMaterialize = Config.bMaterializeLongEdges;
	Config.bMaterializeLongEdges = false;
	bool bLaidOut = false;
#if BPAL_MATERIAL_TOOLS
	// Comments form layout blocks. Solve each block internally, then solve the block graph.
	// Overlapping/nested comments share one block so no node is moved twice.
	TArray<UEdGraphNode*> RealNodes;
	TMap<UEdGraphNode*, int32> Index;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		const FLayoutNodeInfo* Info = NodeInfoMap.Find(Node);
		if (Info && !Info->bIsComment) { Index.Add(Node, RealNodes.Num()); RealNodes.Add(Node); }
	}
	TArray<int32> Parent;
	for (int32 I = 0; I < RealNodes.Num(); ++I) Parent.Add(I);
	auto Find = [&Parent](int32 I) { while (Parent[I] != I) I = Parent[I]; return I; };
	TSet<UEdGraphNode*> Frozen;
	const bool bPreserve = GetDefault<UBlueprintAutoLayoutSettings>()->bPreserveMaterialCommentGroups;
	for (auto& Pair : CommentMembers)
	{
		const bool bPinned = BPALMaterialTools::IsPinned(Pair.Key);
		if (bPinned) for (UEdGraphNode* Member : Pair.Value) Frozen.Add(Member);
		if (!bPreserve && !bPinned) continue;
		if (Selection && !Selection->Contains(Pair.Key)) continue;
		int32 First = INDEX_NONE;
		for (UEdGraphNode* Member : Pair.Value)
		{
			if (const int32* I = Index.Find(Member))
			{
				if (First == INDEX_NONE) First = *I;
				else Parent[Find(*I)] = Find(First);
			}
		}
	}
	struct FBlock
	{
		TArray<UEdGraphNode*> Nodes;
		FVector2D Origin = FVector2D::ZeroVector;
		FVector2D Size = FVector2D::ZeroVector;
		bool bFixed = false;
		int32 CommentLayers = 0;
	};
	TArray<FBlock> Blocks;
	TMap<int32, int32> BlockForRoot;
	TMap<UEdGraphNode*, int32> BlockForNode;
	TMap<UEdGraphNode*, FVector2D> Local;
	for (int32 I = 0; I < RealNodes.Num(); ++I)
	{
		const int32 Root = Find(I);
		int32* Existing = BlockForRoot.Find(Root);
		const int32 BlockIndex = Existing ? *Existing : Blocks.AddDefaulted();
		if (!Existing) BlockForRoot.Add(Root, BlockIndex);
		Blocks[BlockIndex].Nodes.Add(RealNodes[I]);
		Blocks[BlockIndex].bFixed |= Frozen.Contains(RealNodes[I]) || BPALMaterialTools::IsPinned(RealNodes[I]);
		BlockForNode.Add(RealNodes[I], BlockIndex);
	}
	for (auto& Pair : CommentMembers)
	{
		if ((!bPreserve && !BPALMaterialTools::IsPinned(Pair.Key)) || (Selection && !Selection->Contains(Pair.Key))) continue;
		for (UEdGraphNode* Member : Pair.Value)
		{
			if (const int32* Block = BlockForNode.Find(Member)) { ++Blocks[*Block].CommentLayers; break; }
		}
	}
	auto ImplicitDependency = [](UEdGraphNode* Node) -> UEdGraphNode* {
		UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node);
		UMaterialExpressionNamedRerouteUsage* Usage = MaterialNode ? Cast<UMaterialExpressionNamedRerouteUsage>(MaterialNode->MaterialExpression) : nullptr;
		return Usage && Usage->Declaration ? Usage->Declaration->GraphNode.Get() : nullptr;
	};
	auto SizeOf = [this](UEdGraphNode* Node) {
		const FLayoutNodeInfo& Info = NodeInfoMap.FindChecked(Node);
		return FVector2D(Info.NodeWidth, Info.NodeHeight);
	};
	for (FBlock& Block : Blocks)
	{
		FBox2D Original(ForceInit);
		bpal::FLayeredConfig Cfg;
		Cfg.RankSpacingX = Config.LayeredRankSpacingX;
		Cfg.NodeSpacingY = Config.LayeredNodeSpacingY;
		bpal::FLayeredGraph Solve(Cfg);
		TMap<UEdGraphNode*, int32> Id;
		for (UEdGraphNode* Node : Block.Nodes)
		{
			const FVector2D Size = SizeOf(Node);
			Id.Add(Node, Solve.AddVertex(Size.X, Size.Y));
			Original += FVector2D(Node->NodePosX, Node->NodePosY);
			Original += FVector2D(Node->NodePosX, Node->NodePosY) + Size;
		}
		if (!Block.bFixed)
		{
			for (UEdGraphNode* Node : Block.Nodes)
			{
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output) continue;
					for (UEdGraphPin* Link : Pin->LinkedTo) if (Link && Id.Contains(Link->GetOwningNode()))
						Solve.AddEdge(Id[Node], Id[Link->GetOwningNode()], PinYOffset(Node, Pin), PinYOffset(Link->GetOwningNode(), Link), false);
				}
				if (UEdGraphNode* Dependency = ImplicitDependency(Node))
					if (Id.Contains(Dependency)) Solve.AddEdge(Id[Dependency], Id[Node]);
			}
			Solve.Solve();
		}
		Block.Origin = Original.Min;
		FBox2D Bounds(ForceInit);
		for (UEdGraphNode* Node : Block.Nodes)
		{
			const auto& V = Solve.Vertices()[Id[Node]];
			const FVector2D Pos = Block.bFixed ? FVector2D(Node->NodePosX, Node->NodePosY) - Original.Min : FVector2D(V.X, V.Y);
			Local.Add(Node, Pos);
			Bounds += Pos; Bounds += Pos + SizeOf(Node);
		}
		// Reserve comment padding in the outer solve, not just the expression rectangles.
		const FVector2D Pad(Block.CommentLayers * Config.CommentPadding, Block.CommentLayers * (Config.CommentPadding + Config.CommentTitleHeight));
		for (UEdGraphNode* Node : Block.Nodes) Local[Node] += Pad - Bounds.Min;
		Block.Origin -= Pad;
		Block.Size = Bounds.GetSize() + Pad + FVector2D(Pad.X, Block.CommentLayers * Config.CommentPadding);
	}
	bpal::FLayeredConfig OuterCfg;
	OuterCfg.RankSpacingX = Config.LayeredRankSpacingX;
	OuterCfg.NodeSpacingY = Config.LayeredNodeSpacingY;
	bpal::FLayeredGraph Outer(OuterCfg);
	for (const FBlock& Block : Blocks) Outer.AddVertex(Block.Size.X, Block.Size.Y);
	for (UEdGraphNode* Node : RealNodes)
	{
		const int32 From = BlockForNode[Node];
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output) continue;
			for (UEdGraphPin* Link : Pin->LinkedTo)
			{
				if (!Link || !BlockForNode.Contains(Link->GetOwningNode())) continue;
				const int32 To = BlockForNode[Link->GetOwningNode()];
				if (From != To) Outer.AddEdge(From, To, Local[Node].Y + PinYOffset(Node, Pin),
					Local[Link->GetOwningNode()].Y + PinYOffset(Link->GetOwningNode(), Link), false);
			}
		}
		if (UEdGraphNode* Dependency = ImplicitDependency(Node))
			if (BlockForNode.Contains(Dependency) && BlockForNode[Dependency] != From) Outer.AddEdge(BlockForNode[Dependency], From);
	}
	Outer.Solve();
	TArray<FBox2D> Occupied;
	for (const FBlock& Block : Blocks) if (Block.bFixed) Occupied.Add(FBox2D(Block.Origin, Block.Origin + Block.Size));
	// Selection-only layouts also avoid unselected expressions and comments.
	if (Selection) for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || NodeInfoMap.Contains(Node) || Node->IsA<UEdGraphNode_Comment>()) continue;
		Occupied.Add(FBox2D(FVector2D(Node->NodePosX, Node->NodePosY), FVector2D(Node->NodePosX + FMath::Max(Node->NodeWidth, 220), Node->NodePosY + FMath::Max(Node->NodeHeight, 100))));
	}
	for (int32 I = 0; I < Blocks.Num(); ++I)
	{
		FBlock& Block = Blocks[I];
		if (!Block.bFixed)
		{
			const auto& V = Outer.Vertices()[I];
			Block.Origin = FVector2D(StartX + V.X, StartY + V.Y);
			bool bMoved;
			do
			{
				bMoved = false;
				for (const FBox2D& Box : Occupied)
				{
					if (FBox2D(Block.Origin, Block.Origin + Block.Size).Intersect(Box.ExpandBy(Config.LayeredNodeSpacingY)))
					{
						Block.Origin.Y = Box.Max.Y + Config.LayeredNodeSpacingY + 1;
						bMoved = true;
					}
				}
			} while (bMoved);
			Occupied.Add(FBox2D(Block.Origin, Block.Origin + Block.Size));
		}
		for (UEdGraphNode* Node : Block.Nodes)
		{
			FLayoutNodeInfo& Info = NodeInfoMap.FindChecked(Node);
			Info.LayoutX = FMath::RoundToInt(Block.Origin.X + Local[Node].X);
			Info.LayoutY = FMath::RoundToInt(Block.Origin.Y + Local[Node].Y);
			Info.bPositioned = !Block.bFixed;
		}
	}
	// A pinned comment freezes both its bounds and its contents.
	AllComments.RemoveAll([&Frozen](FLayoutNodeInfo* Info) { return Frozen.Contains(Info->Node) || BPALMaterialTools::IsPinned(Info->Node); });
	bLaidOut = !RealNodes.IsEmpty();
#else
	bLaidOut = AssignPositionsLayered(StartX, StartY);
#endif
	Config.bMaterializeLongEdges = bSavedMaterialize;
	if (!bLaidOut) return 0;

	// ResizeNode on a material comment writes into its expression immediately, so capture
	// every expression BEFORE applying positions or resizing comments for correct undo.
	for (auto& Pair : NodeInfoMap)
	{
		if (UMaterialGraphNode_Base* Node = Cast<UMaterialGraphNode_Base>(Pair.Key))
		{
			if (UObject* Owner = Node->GetMaterialNodeOwner()) Owner->Modify();
		}
		else if (UMaterialGraphNode_Comment* Comment = Cast<UMaterialGraphNode_Comment>(Pair.Key))
		{
			if (Comment->MaterialExpressionComment) Comment->MaterialExpressionComment->Modify();
		}
	}
	ApplyPositions();
	// Resize inner comments first, then let outer comments enclose their new bounds.
	AllComments.Sort([](const FLayoutNodeInfo& A, const FLayoutNodeInfo& B) {
		return int64(A.Node->NodeWidth) * A.Node->NodeHeight < int64(B.Node->NodeWidth) * B.Node->NodeHeight;
	});
	const auto Comments = AllComments;
	for (FLayoutNodeInfo* Comment : Comments)
	{
		AllComments = { Comment };
		WrapComments();
		Comment->NodeWidth = Comment->Node->NodeWidth;
		Comment->NodeHeight = Comment->Node->NodeHeight;
	}
	AllComments = Comments;

	// Persist transient graph-node positions on their expression owners in the same undo step.
	int32 Count = 0;
	for (auto& Pair : NodeInfoMap)
	{
		UEdGraphNode* Node = Pair.Key;
		if (!Pair.Value.bPositioned && !Pair.Value.bIsComment) continue;
		if (UMaterialGraphNode* ExpressionNode = Cast<UMaterialGraphNode>(Node))
		{
			if (UMaterialExpression* Expression = ExpressionNode->MaterialExpression)
			{
				Expression->MaterialExpressionEditorX = Node->NodePosX;
				Expression->MaterialExpressionEditorY = Node->NodePosY;
			}
		}
		else if (UMaterialGraphNode_Comment* Comment = Cast<UMaterialGraphNode_Comment>(Node))
		{
			if (UMaterialExpressionComment* Expression = Comment->MaterialExpressionComment)
			{
				Expression->MaterialExpressionEditorX = Node->NodePosX;
				Expression->MaterialExpressionEditorY = Node->NodePosY;
				Expression->SizeX = Node->NodeWidth;
				Expression->SizeY = Node->NodeHeight;
			}
		}
		else if (Node->IsA<UMaterialGraphNode_Root>() && MaterialGraph->Material)
		{
			MaterialGraph->Material->EditorX = Node->NodePosX;
			MaterialGraph->Material->EditorY = Node->NodePosY;
		}
		++Count;
	}
	MaterialGraph->MaterialDirtyDelegate.ExecuteIfBound();
	return Count;
}
