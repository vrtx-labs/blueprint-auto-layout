// Copyright (c) 2026 Alex Coulombe. Licensed under the MIT License.
#include "BPALMaterialTools.h"

#if BPAL_MATERIAL_TOOLS
#include "BlueprintAutoLayout.h"
#include "BlueprintAutoLayoutSettings.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Comment.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialFunction.h"
#include "ScopedTransaction.h"
#include "GraphEditor.h"
#include "SGraphPanel.h"
#include "SGraphNode.h"

namespace BPALMaterialTools
{
static const FString RouteTag(TEXT("BPAL:GeneratedMaterialRoute"));
static const FString GroupTag(TEXT("BPAL:MaterialOutput:"));

static UMaterialExpression* ExpressionFor(UEdGraphNode* Node)
{
	if (UMaterialGraphNode* N = Cast<UMaterialGraphNode>(Node)) return N->MaterialExpression;
	if (UMaterialGraphNode_Comment* N = Cast<UMaterialGraphNode_Comment>(Node)) return N->MaterialExpressionComment;
	return nullptr;
}

static FString PinKey(UEdGraphNode* Node)
{
	if (!Node) return {};
	UMaterialGraph* Graph = Cast<UMaterialGraph>(Node->GetGraph());
	if (!Graph) return {};
	const FString Asset = Graph->OriginalMaterialFullName.IsEmpty()
		? Graph->GetMaterialOrFunction()->GetPathName() : Graph->OriginalMaterialFullName;
	if (UMaterialExpression* Expression = ExpressionFor(Node))
	{
		return Asset + TEXT(":") + Expression->MaterialExpressionGuid.ToString();
	}
	return Node->IsA<UMaterialGraphNode_Root>() ? Asset + TEXT(":MaterialOutput") : FString();
}

bool IsPinned(UEdGraphNode* Node)
{
	const FString Key = PinKey(Node);
	return !Key.IsEmpty() && GetDefault<UBlueprintAutoLayoutSettings>()->PinnedMaterialNodes.Contains(Key);
}

static bool IsInPinnedGroup(UEdGraphNode* Node)
{
	if (!Node) return false;
	for (UEdGraphNode* Other : Node->GetGraph()->Nodes)
	{
		if (!Other || !Other->IsA<UEdGraphNode_Comment>() || !IsPinned(Other)) continue;
		if (Node->NodePosX >= Other->NodePosX && Node->NodePosY >= Other->NodePosY &&
			Node->NodePosX <= Other->NodePosX + Other->NodeWidth && Node->NodePosY <= Other->NodePosY + Other->NodeHeight) return true;
	}
	return false;
}

void SetPinned(const TArray<UEdGraphNode*>& Nodes, bool bPinned, bool bSave)
{
	UBlueprintAutoLayoutSettings* Settings = GetMutableDefault<UBlueprintAutoLayoutSettings>();
	for (UEdGraphNode* Node : Nodes)
	{
		if (!Node) continue;
		if (UMaterialExpression* Expression = ExpressionFor(Node))
		{
			if (!Expression->MaterialExpressionGuid.IsValid())
			{
				Expression->Modify();
				Expression->UpdateMaterialExpressionGuid(true, true);
			}
		}
		const FString Key = PinKey(Node);
		if (Key.IsEmpty()) continue;
		if (bPinned) Settings->PinnedMaterialNodes.AddUnique(Key);
		else Settings->PinnedMaterialNodes.Remove(Key);
	}
	if (bSave) Settings->SaveConfig();
}

void GatherDependencies(UEdGraphNode* Node, TArray<UEdGraphNode*>& Dependencies)
{
	if (!Node) return;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input) continue;
		for (UEdGraphPin* Link : Pin->LinkedTo)
		{
			if (Link && Link->GetOwningNode()->GetGraph() == Node->GetGraph())
				Dependencies.AddUnique(Link->GetOwningNode());
		}
	}
	// Named reroutes have an implicit dependency rather than a visible graph link.
	if (UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(ExpressionFor(Node)))
	{
		if (Usage->Declaration && Usage->Declaration->GraphNode && Usage->Declaration->GraphNode->GetGraph() == Node->GetGraph())
			Dependencies.AddUnique(Usage->Declaration->GraphNode);
	}
}

TArray<UEdGraphNode*> CollectUpstream(UMaterialGraph* Graph, const TArray<UEdGraphNode*>& Seeds)
{
	TArray<UEdGraphNode*> Result, Pending = Seeds;
	TSet<UEdGraphNode*> Seen;
	while (!Pending.IsEmpty())
	{
		UEdGraphNode* Node = Pending.Pop(EAllowShrinking::No);
		if (!Node || Node->GetGraph() != Graph || Seen.Contains(Node)) continue;
		Seen.Add(Node);
		if (UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			for (UEdGraphNode* Other : Graph->Nodes)
			{
				if (Other && Other != Node && Other->NodePosX >= Node->NodePosX && Other->NodePosY >= Node->NodePosY &&
					Other->NodePosX + FMath::Max(Other->NodeWidth, 1) <= Node->NodePosX + Comment->NodeWidth &&
					Other->NodePosY + FMath::Max(Other->NodeHeight, 1) <= Node->NodePosY + Comment->NodeHeight)
					Pending.Add(Other);
			}
		}
		Result.Add(Node);
		GatherDependencies(Node, Pending);
	}
	return Result;
}

static FMaterialExpressionCollection& Collection(UMaterialGraph* Graph)
{
	return Graph->Material ? Graph->Material->GetExpressionCollection() : Graph->MaterialFunction->GetExpressionCollection();
}

static UMaterialExpression* NewExpression(UMaterialGraph* Graph, UClass* Class)
{
	UObject* Owner = Graph->Material ? static_cast<UObject*>(Graph->Material) : Graph->MaterialFunction;
	Owner->Modify();
	Graph->Modify();
	UMaterialExpression* Expression = NewObject<UMaterialExpression>(Owner, Class, NAME_None, RF_Transactional);
	Expression->Material = Graph->Material;
	Expression->SubgraphExpression = Graph->SubgraphExpression;
	Expression->UpdateMaterialExpressionGuid(true, true);
	if (UMaterialExpressionComment* Comment = Cast<UMaterialExpressionComment>(Expression)) Collection(Graph).AddComment(Comment);
	else Collection(Graph).AddExpression(Expression);
	return Expression;
}

void GroupOutputs(UMaterialGraph* Graph)
{
	struct FOutput { FString Name; TArray<UEdGraphNode*> Members; };
	TArray<FOutput> Outputs;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		const bool bRoot = Node && Node->IsA<UMaterialGraphNode_Root>();
		if (!bRoot && !Cast<UMaterialExpressionFunctionOutput>(ExpressionFor(Node))) continue;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->LinkedTo.IsEmpty()) continue;
			TArray<UEdGraphNode*> Seeds;
			for (UEdGraphPin* Link : Pin->LinkedTo) if (Link) Seeds.Add(Link->GetOwningNode());
			const FString Name = bRoot ? Pin->GetDisplayName().ToString() : Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			Outputs.Add({Name, CollectUpstream(Graph, Seeds)});
		}
	}
	TMap<UEdGraphNode*, int32> Uses;
	for (const FOutput& Output : Outputs) for (UEdGraphNode* Node : Output.Members) Uses.FindOrAdd(Node)++;
	TArray<UEdGraphNode*> Shared;
	for (auto& Pair : Uses) if (Pair.Value > 1) Shared.Add(Pair.Key);
	for (FOutput& Output : Outputs) Output.Members.RemoveAll([&Uses](UEdGraphNode* Node) { return Uses[Node] > 1; });
	if (!Shared.IsEmpty()) Outputs.Add({TEXT("Shared Dependencies"), Shared});

	// Respect hand-authored groups. Plugin-created groups are reused, never accumulated.
	TSet<UEdGraphNode*> ManuallyGrouped;
	int32 NextGroupY = 0;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node) NextGroupY = FMath::Max(NextGroupY, Node->NodePosY + FMath::Max(Node->NodeHeight, 100) + 200);
		UMaterialGraphNode_Comment* Comment = Cast<UMaterialGraphNode_Comment>(Node);
		if (!Comment || !Comment->MaterialExpressionComment || (Comment->MaterialExpressionComment->Desc.StartsWith(GroupTag) && !IsPinned(Comment))) continue;
		for (auto& Pair : Uses)
		{
			UEdGraphNode* Member = Pair.Key;
			if (Member->NodePosX >= Comment->NodePosX && Member->NodePosY >= Comment->NodePosY &&
				Member->NodePosX < Comment->NodePosX + Comment->NodeWidth && Member->NodePosY < Comment->NodePosY + Comment->NodeHeight)
				ManuallyGrouped.Add(Member);
		}
	}
	for (const FOutput& Output : Outputs)
	{
		TArray<UEdGraphNode*> Members = Output.Members;
		Members.RemoveAll([&](UEdGraphNode* Node) { return ManuallyGrouped.Contains(Node) || IsPinned(Node) || Node->IsA<UEdGraphNode_Comment>(); });
		UMaterialGraphNode_Comment* Comment = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UMaterialGraphNode_Comment* Existing = Cast<UMaterialGraphNode_Comment>(Node);
			if (Existing && Existing->MaterialExpressionComment && Existing->MaterialExpressionComment->Desc == GroupTag + Output.Name)
			{
				Comment = Existing;
				break;
			}
		}
		if (Members.IsEmpty() || (Comment && IsPinned(Comment))) continue;
		if (!Comment)
		{
			UMaterialExpressionComment* Expression = CastChecked<UMaterialExpressionComment>(NewExpression(Graph, UMaterialExpressionComment::StaticClass()));
			Expression->Text = Output.Name;
			Expression->Desc = GroupTag + Output.Name;
			const uint32 Hash = GetTypeHash(Output.Name);
			Expression->CommentColor = FLinearColor::MakeFromHSV8(Hash % 255, 140, 190);
			Comment = Graph->AddComment(Expression, false);
			Comment->SetFlags(RF_Transactional);
		}
		Comment->Modify();
		UMaterialExpressionComment* Expression = Comment->MaterialExpressionComment;
		Expression->Modify();
		// Separate new output groups before the block layout. Otherwise interleaved branches
		// can create overlapping comments whose geometric memberships are ambiguous.
		int32 MinY = MAX_int32;
		for (UEdGraphNode* Node : Members) MinY = FMath::Min(MinY, Node->NodePosY);
		for (UEdGraphNode* Node : Members)
		{
			Node->Modify();
			Node->NodePosY += NextGroupY - MinY;
			if (UMaterialExpression* MemberExpression = ExpressionFor(Node))
			{
				MemberExpression->Modify();
				MemberExpression->MaterialExpressionEditorY = Node->NodePosY;
			}
		}
		int32 L = MAX_int32, T = MAX_int32, R = MIN_int32, B = MIN_int32;
		for (UEdGraphNode* Node : Members)
		{
			int32 W = FMath::Max(Node->NodeWidth, 220), H = FMath::Max(Node->NodeHeight, 100);
			if (TSharedPtr<SGraphEditor> Editor = SGraphEditor::FindGraphEditorForGraph(Graph))
			{
				if (TSharedPtr<SGraphNode> Widget = Editor->GetGraphPanel()->GetNodeWidgetFromGuid(Node->NodeGuid))
				{
					W = FMath::Max(W, FMath::CeilToInt(Widget->GetDesiredSize().X));
					H = FMath::Max(H, FMath::CeilToInt(Widget->GetDesiredSize().Y));
				}
			}
			L = FMath::Min(L, Node->NodePosX); T = FMath::Min(T, Node->NodePosY);
			R = FMath::Max(R, Node->NodePosX + W); B = FMath::Max(B, Node->NodePosY + H);
		}
		Comment->NodePosX = Expression->MaterialExpressionEditorX = L - 36;
		Comment->NodePosY = Expression->MaterialExpressionEditorY = T - 76;
		Comment->ResizeNode(FVector2f(R - L + 72, B - T + 112));
		Expression->SizeX = Comment->NodeWidth;
		Expression->SizeY = Comment->NodeHeight;
		NextGroupY = Comment->NodePosY + Comment->NodeHeight + 200;
	}
	const auto Nodes = Graph->Nodes;
	for (UEdGraphNode* Node : Nodes)
	{
		auto* Comment = Cast<UMaterialGraphNode_Comment>(Node);
		if (!Comment || !Comment->MaterialExpressionComment || IsPinned(Comment)) continue;
		const FString Tag = Comment->MaterialExpressionComment->Desc;
		if (!Tag.StartsWith(GroupTag) || Outputs.ContainsByPredicate([&](const FOutput& O) { return Tag == GroupTag + O.Name; })) continue;
		Comment->Modify(); Comment->MaterialExpressionComment->Modify();
		Graph->Modify();
		(Graph->Material ? static_cast<UObject*>(Graph->Material) : Graph->MaterialFunction)->Modify();
		Collection(Graph).RemoveComment(Comment->MaterialExpressionComment);
		Graph->RemoveNode(Comment);
	}
}

void RemoveGeneratedRoutes(UMaterialGraph* Graph)
{
	const auto Nodes = Graph->Nodes;
	for (UEdGraphNode* Node : Nodes)
	{
		UMaterialGraphNode* RoutingNode = Cast<UMaterialGraphNode>(Node);
		UMaterialExpressionReroute* Expression = RoutingNode ? Cast<UMaterialExpressionReroute>(RoutingNode->MaterialExpression) : nullptr;
		if (!Expression || Expression->Desc != RouteTag || IsPinned(Node) || IsInPinnedGroup(Node)) continue;
		UEdGraphPin* Input = RoutingNode->GetInputPin(0);
		UEdGraphPin* Output = RoutingNode->GetOutputPin(0);
		if (!Input || !Output || Input->LinkedTo.Num() != 1) continue;
		UEdGraphPin* Source = Input->LinkedTo[0];
		const auto Consumers = Output->LinkedTo;
		Node->Modify(); Expression->Modify(); Source->GetOwningNode()->Modify();
		Graph->Modify(); Graph->GetMaterialOrFunction()->Modify();
		if (Graph->Material) Graph->Material->Modify();
		for (UEdGraphPin* Consumer : Consumers)
		{
			Consumer->GetOwningNode()->Modify();
			if (UMaterialExpression* Owner = ExpressionFor(Consumer->GetOwningNode())) Owner->Modify();
			Output->BreakLinkTo(Consumer);
			Source->MakeLinkTo(Consumer);
		}
		Input->BreakAllPinLinks();
		Collection(Graph).RemoveExpression(Expression);
		Graph->RemoveNode(Node);
	}
	Graph->LinkMaterialExpressionsFromGraph();
}

struct FObstacle { UEdGraphNode* Node; FBox2D Box; };

static bool SegmentHits(const FVector2D& A, const FVector2D& B, const FBox2D& Box)
{
	if (FMath::IsNearlyEqual(A.Y, B.Y))
		return A.Y >= Box.Min.Y && A.Y <= Box.Max.Y && FMath::Max(A.X, B.X) >= Box.Min.X && FMath::Min(A.X, B.X) <= Box.Max.X;
	if (FMath::IsNearlyEqual(A.X, B.X))
		return A.X >= Box.Min.X && A.X <= Box.Max.X && FMath::Max(A.Y, B.Y) >= Box.Min.Y && FMath::Min(A.Y, B.Y) <= Box.Max.Y;
	// Bounding-box rejection followed by a slab intersection for the original diagonal wire.
	float Lo = 0.f, Hi = 1.f;
	for (int32 Axis = 0; Axis < 2; ++Axis)
	{
		const double Delta = B[Axis] - A[Axis];
		if (FMath::IsNearlyZero(Delta)) { if (A[Axis] < Box.Min[Axis] || A[Axis] > Box.Max[Axis]) return false; }
		else
		{
			double P = (Box.Min[Axis] - A[Axis]) / Delta, Q = (Box.Max[Axis] - A[Axis]) / Delta;
			if (P > Q) Swap(P, Q);
			Lo = FMath::Max(Lo, float(P)); Hi = FMath::Min(Hi, float(Q));
			if (Lo > Hi) return false;
		}
	}
	return true;
}

void RouteWires(UMaterialGraph* Graph)
{
	TArray<FObstacle> Obstacles;
	float Top = 0, Bottom = 0;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || Node->IsA<UEdGraphNode_Comment>()) continue;
		FVector2D Size(FMath::Max(Node->NodeWidth, 220), FMath::Max(Node->NodeHeight, 100));
		if (TSharedPtr<SGraphEditor> Editor = SGraphEditor::FindGraphEditorForGraph(Graph))
			if (TSharedPtr<SGraphNode> Widget = Editor->GetGraphPanel()->GetNodeWidgetFromGuid(Node->NodeGuid)) Size = Widget->GetDesiredSize();
		FBox2D Box(FVector2D(Node->NodePosX, Node->NodePosY), FVector2D(Node->NodePosX, Node->NodePosY) + Size);
		Obstacles.Add({Node, Box}); Top = FMath::Min(Top, float(Box.Min.Y)); Bottom = FMath::Max(Bottom, float(Box.Max.Y));
	}
	struct FWire { UEdGraphPin* Source; UEdGraphPin* Dest; };
	TArray<FWire> Wires;
	for (UEdGraphNode* Node : Graph->Nodes) if (Node) for (UEdGraphPin* Pin : Node->Pins)
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UMaterialGraphSchema::PC_Exec)
			for (UEdGraphPin* Link : Pin->LinkedTo) if (Link) Wires.Add({Pin, Link});
	int32 Lane = 0;
	for (const FWire& Wire : Wires)
	{
		UEdGraphNode* SourceNode = Wire.Source->GetOwningNode();
		UEdGraphNode* DestNode = Wire.Dest->GetOwningNode();
		if (IsInPinnedGroup(SourceNode) || IsInPinnedGroup(DestNode)) continue;
		const FObstacle* Source = Obstacles.FindByPredicate([&](const FObstacle& O) { return O.Node == SourceNode; });
		const FObstacle* Dest = Obstacles.FindByPredicate([&](const FObstacle& O) { return O.Node == DestNode; });
		if (!Source || !Dest) continue;
		auto PinY = [](UEdGraphPin* Pin) {
			int32 Index = 0;
			for (UEdGraphPin* P : Pin->GetOwningNode()->Pins)
			{
				if (P == Pin) break;
				if (P && !P->bHidden && P->Direction == Pin->Direction) ++Index;
			}
			return double(Pin->GetOwningNode()->NodePosY + 40 + 26 * Index);
		};
		const FVector2D A(Source->Box.Max.X, PinY(Wire.Source)), B(Dest->Box.Min.X, PinY(Wire.Dest));
		bool bBlocked = false;
		for (const FObstacle& O : Obstacles) if (O.Node != SourceNode && O.Node != DestNode && SegmentHits(A, B, O.Box.ExpandBy(12))) { bBlocked = true; break; }
		if (!bBlocked && B.X - A.X < 2 * GetDefault<UBlueprintAutoLayoutSettings>()->HorizontalSpacing + 220) continue;
		TArray<FVector2D> Path;
		for (int32 Attempt = 0; Attempt < 12 && Path.IsEmpty(); ++Attempt)
		{
			const double Y = (Attempt % 2 == 0) ? Top - 60 - Lane * 32 : Bottom + 60 + Lane * 32;
			const double Offset = 20 + (Attempt / 2) * 16;
			TArray<FVector2D> Candidate = { A, {A.X + Offset, A.Y}, {A.X + Offset, Y}, {B.X - Offset, Y}, {B.X - Offset, B.Y}, B };
			bool bClear = true;
			for (const FObstacle& O : Obstacles)
			{
				if (O.Node == SourceNode || O.Node == DestNode) continue;
				for (int32 I = 1; I < Candidate.Num(); ++I) if (SegmentHits(Candidate[I-1], Candidate[I], O.Box.ExpandBy(12))) { bClear = false; break; }
				if (!bClear) break;
			}
			if (bClear) Path = MoveTemp(Candidate);
		}
		if (Path.IsEmpty()) continue;
		++Lane;
		SourceNode->Modify(); DestNode->Modify();
		if (UMaterialExpression* Owner = ExpressionFor(DestNode)) Owner->Modify();
		Wire.Source->BreakLinkTo(Wire.Dest);
		UEdGraphPin* Previous = Wire.Source;
		for (int32 I = 1; I + 1 < Path.Num(); ++I)
		{
			UMaterialExpressionReroute* Expression = CastChecked<UMaterialExpressionReroute>(NewExpression(Graph, UMaterialExpressionReroute::StaticClass()));
			Expression->Desc = RouteTag;
			Expression->MaterialExpressionEditorX = FMath::RoundToInt(Path[I].X);
			Expression->MaterialExpressionEditorY = FMath::RoundToInt(Path[I].Y);
			UMaterialGraphNode* Node = Graph->AddExpression(Expression, false);
			Node->SetFlags(RF_Transactional);
			Previous->MakeLinkTo(Node->GetInputPin(0));
			Previous = Node->GetOutputPin(0);
		}
		Previous->MakeLinkTo(Wire.Dest);
	}
	Graph->LinkMaterialExpressionsFromGraph();
	Graph->UpdatePinTypes();
}

void Run(UMaterialGraph* Graph, EAction Action, const TArray<UEdGraphNode*>& Seeds)
{
	if (!Graph) return;
	TArray<UEdGraphNode*> Nodes = Action == EAction::Chain ? CollectUpstream(Graph, Seeds) : Seeds;
	const bool bSubset = Action == EAction::Selected || Action == EAction::Chain;
	Nodes.RemoveAll([Graph](UEdGraphNode* Node) { return !Node || Node->GetGraph() != Graph; });
	if (bSubset && Nodes.IsEmpty()) return;
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintAutoLayout", "MaterialLayoutTransaction", "Auto Layout Material Graph"));
	Graph->Modify();
	Graph->GetMaterialOrFunction()->Modify();
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		Node->Modify();
		if (UMaterialExpression* Expression = ExpressionFor(Node)) Expression->Modify();
	}
	const UBlueprintAutoLayoutSettings* Settings = GetDefault<UBlueprintAutoLayoutSettings>();
	FBlueprintLayoutConfig Config;
	Config.LayeredRankSpacingX = Settings->HorizontalSpacing;
	Config.LayeredNodeSpacingY = Settings->VerticalSpacing;
	FBlueprintAutoLayout Layout(Config);
	if (bSubset)
	{
		int32 X = MAX_int32, Y = MAX_int32;
		for (UEdGraphNode* Node : Nodes) { X = FMath::Min(X, Node->NodePosX); Y = FMath::Min(Y, Node->NodePosY); }
		Layout.LayoutSubtree(Graph, Nodes, X, Y);
	}
	else
	{
		switch (Action)
		{
		case EAction::Group: Layout.LayoutAndGroupGraph(Graph); break;
		case EAction::Route: Layout.LayoutAndRouteGraph(Graph); break;
		case EAction::GroupAndRoute: Layout.LayoutGroupAndRouteGraph(Graph); break;
		default: Layout.LayoutGraph(Graph); break;
		}
	}
	Graph->MaterialDirtyDelegate.ExecuteIfBound();
	Graph->NotifyGraphChanged();
	if (Action == EAction::Route || Action == EAction::GroupAndRoute) RefreshOpenEditor(Graph);
	if (Settings->bFrameMaterialLayout)
	{
		if (TSharedPtr<SGraphEditor> Editor = SGraphEditor::FindGraphEditorForGraph(Graph))
		{
			if (!bSubset) Editor->ZoomToFit(false);
			else
			{
				FBox2D Bounds(ForceInit);
				for (UEdGraphNode* Node : Nodes)
				{
					Bounds += FVector2D(Node->NodePosX, Node->NodePosY);
					Bounds += FVector2D(Node->NodePosX + FMath::Max(Node->NodeWidth, 220), Node->NodePosY + FMath::Max(Node->NodeHeight, 100));
				}
				const FVector2D ViewSize = Editor->GetGraphPanel()->GetCachedGeometry().GetLocalSize();
				const FVector2D Size = Bounds.GetSize() + FVector2D(100, 100);
				const float Zoom = FMath::Clamp(float(FMath::Min(ViewSize.X / Size.X, ViewSize.Y / Size.Y)), 0.1f, 1.f);
				Editor->SetViewLocation(FVector2f(Bounds.GetCenter() - ViewSize / (2 * Zoom)), Zoom);
			}
		}
	}
}
}
#endif
