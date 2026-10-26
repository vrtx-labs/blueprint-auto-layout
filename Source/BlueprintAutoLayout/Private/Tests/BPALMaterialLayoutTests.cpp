// Copyright (c) 2026 Alex Coulombe. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
#include "BlueprintAutoLayoutModule.h"
#include "Editor.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Comment.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialFunction.h"
#include "BlueprintAutoLayout.h"
#include "UObject/StrongObjectPtr.h"
#include "BPALMaterialTools.h"
#include "BlueprintAutoLayoutSettings.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "ScopedTransaction.h"
#include "MaterialEditorModule.h"
#include "IMaterialEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolMenus.h"
#include "ToolMenu.h"
#include "Toolkits/AssetEditorToolkitMenuContext.h"
#include "GraphEditor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBPALMaterialLayoutTest, "BlueprintAutoLayout.Material.LayoutAndUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBPALMaterialLayoutTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transactional));
	UMaterialGraph* Graph = NewObject<UMaterialGraph>(Material.Get(), NAME_None, RF_Transactional);
	Graph->Material = Material.Get();
	Graph->Schema = UMaterialGraphSchema::StaticClass();
	Material->MaterialGraph = Graph;
	Graph->RebuildGraph();
	UMaterialExpressionConstant* Source = NewObject<UMaterialExpressionConstant>(Material.Get(), NAME_None, RF_Transactional);
	UMaterialExpressionMultiply* Consumer = NewObject<UMaterialExpressionMultiply>(Material.Get(), NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Source);
	Material->GetExpressionCollection().AddExpression(Consumer);
	UMaterialGraphNode* A = Graph->AddExpression(Source, false);
	UMaterialGraphNode* B = Graph->AddExpression(Consumer, false);
	A->SetFlags(RF_Transactional);
	B->SetFlags(RF_Transactional);
	A->NodePosX = Source->MaterialExpressionEditorX = 700;
	B->NodePosX = Consumer->MaterialExpressionEditorX = -300;
	A->NodePosY = Source->MaterialExpressionEditorY = 100;
	B->NodePosY = Consumer->MaterialExpressionEditorY = 100;
	A->GetOutputPin(0)->MakeLinkTo(B->GetInputPin(0));

	UMaterialExpressionComment* CommentExpression = NewObject<UMaterialExpressionComment>(Material.Get(), NAME_None, RF_Transactional);
	UMaterialGraphNode_Comment* Comment = Graph->AddComment(CommentExpression, false);
	Comment->SetFlags(RF_Transactional);
	Comment->NodePosX = -500;
	Comment->NodePosY = -100;
	Comment->NodeWidth = CommentExpression->SizeX = 1600;
	Comment->NodeHeight = CommentExpression->SizeY = 600;
	const int32 NodeCount = Graph->Nodes.Num();
	FBlueprintAutoLayoutModule::ExecuteLayoutOnGraph(Graph);
	TestTrue(TEXT("Data flows left to right"), A->NodePosX < B->NodePosX);
	TestEqual(TEXT("No K2 knots or extra comments inserted"), Graph->Nodes.Num(), NodeCount);
	TestTrue(TEXT("Original connection preserved"), A->GetOutputPin(0)->LinkedTo.Contains(B->GetInputPin(0)));
	TestEqual(TEXT("Expression X persisted"), Source->MaterialExpressionEditorX, A->NodePosX);
	TestEqual(TEXT("Expression Y persisted"), Consumer->MaterialExpressionEditorY, B->NodePosY);
	TestEqual(TEXT("Comment position persisted"), CommentExpression->MaterialExpressionEditorX, Comment->NodePosX);
	TestEqual(TEXT("Comment size persisted"), CommentExpression->SizeX, Comment->NodeWidth);
	TestEqual(TEXT("Root position persisted"), Material->EditorX, Graph->RootNode->NodePosX);
	TestTrue(TEXT("Single undo succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("Undo restores graph position"), A->NodePosX, 700);
	TestEqual(TEXT("Undo restores saved expression position"), Source->MaterialExpressionEditorX, 700);
	TestEqual(TEXT("Undo restores saved comment size"), CommentExpression->SizeX, 1600);

	// Selection-only must not move the consumer, root or enclosing unselected comment.
	const FIntPoint OldB(B->NodePosX, B->NodePosY);
	const FIntPoint OldRoot(Graph->RootNode->NodePosX, Graph->RootNode->NodePosY);
	const FIntPoint OldComment(Comment->NodePosX, Comment->NodePosY);
	FBlueprintAutoLayoutModule::ExecuteLayoutSelectedOnGraph(Graph, { A });
	TestEqual(TEXT("Unselected consumer stays put"), FIntPoint(B->NodePosX, B->NodePosY), OldB);
	TestEqual(TEXT("Unselected root stays put"), FIntPoint(Graph->RootNode->NodePosX, Graph->RootNode->NodePosY), OldRoot);
	TestEqual(TEXT("Unselected comment stays put"), FIntPoint(Comment->NodePosX, Comment->NodePosY), OldComment);
	TestTrue(TEXT("Selection preserves boundary connection"), A->GetOutputPin(0)->LinkedTo.Contains(B->GetInputPin(0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBPALMaterialFunctionLayoutTest, "BlueprintAutoLayout.Material.FunctionAndReroute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBPALMaterialFunctionLayoutTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UMaterialFunction> Function(NewObject<UMaterialFunction>());
	UMaterialGraph* Graph = NewObject<UMaterialGraph>(Function.Get());
	Graph->MaterialFunction = Function.Get();
	Graph->Schema = UMaterialGraphSchema::StaticClass();
	UMaterialExpressionConstant* Source = NewObject<UMaterialExpressionConstant>(Function.Get());
	UMaterialExpressionReroute* Reroute = NewObject<UMaterialExpressionReroute>(Function.Get());
	UMaterialExpressionFunctionOutput* Output = NewObject<UMaterialExpressionFunctionOutput>(Function.Get());
	UMaterialGraphNode* A = Graph->AddExpression(Source, false);
	UMaterialGraphNode* B = Graph->AddExpression(Reroute, false);
	UMaterialGraphNode* C = Graph->AddExpression(Output, false);
	A->GetOutputPin(0)->MakeLinkTo(B->GetInputPin(0));
	B->GetOutputPin(0)->MakeLinkTo(C->GetInputPin(0));
	FBlueprintLayoutConfig Config;
	Config.bUseLayeredEngine = false; // Materials must still use data-flow layout.
	FBlueprintAutoLayout Layout(Config);
	TestEqual(TEXT("Function without an event or material root lays out"), Layout.LayoutGraph(Graph), 3);
	TestTrue(TEXT("Existing material reroute ordered between source and output"),
		A->NodePosX < B->NodePosX && B->NodePosX < C->NodePosX);
	TestEqual(TEXT("Existing reroute expression preserved"), B->MaterialExpression.Get(), static_cast<UMaterialExpression*>(Reroute));
	TestTrue(TEXT("Source link preserved"), A->GetOutputPin(0)->LinkedTo.Contains(B->GetInputPin(0)));
	TestTrue(TEXT("Output link preserved"), B->GetOutputPin(0)->LinkedTo.Contains(C->GetInputPin(0)));
	TestEqual(TEXT("Function output position persisted"), Output->MaterialExpressionEditorX, C->NodePosX);
	return true;
}

namespace
{
struct FMaterialFixture
{
	TStrongObjectPtr<UMaterial> Material;
	UMaterialGraph* Graph;
	TArray<FString> SavedPins;
	FMaterialFixture() : Material(NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transactional))
	{
		SavedPins = GetDefault<UBlueprintAutoLayoutSettings>()->PinnedMaterialNodes;
		Graph = NewObject<UMaterialGraph>(Material.Get(), NAME_None, RF_Transactional);
		Graph->Material = Material.Get(); Graph->Schema = UMaterialGraphSchema::StaticClass();
		Material->MaterialGraph = Graph; Graph->RebuildGraph();
	}
	~FMaterialFixture() { GetMutableDefault<UBlueprintAutoLayoutSettings>()->PinnedMaterialNodes = SavedPins; }
	template<class T> UMaterialGraphNode* Add(int32 X, int32 Y)
	{
		T* Expression = NewObject<T>(Material.Get(), NAME_None, RF_Transactional);
		Expression->Material = Material.Get();
		Expression->UpdateMaterialExpressionGuid(true, false);
		Material->GetExpressionCollection().AddExpression(Expression);
		UMaterialGraphNode* Node = Graph->AddExpression(Expression, false);
		Node->SetFlags(RF_Transactional);
		Node->NodePosX = Expression->MaterialExpressionEditorX = X;
		Node->NodePosY = Expression->MaterialExpressionEditorY = Y;
		Node->NodeWidth = 220; Node->NodeHeight = 100;
		return Node;
	}
	UMaterialGraphNode_Comment* Comment(const TArray<UMaterialGraphNode*>& Nodes)
	{
		UMaterialExpressionComment* Expression = NewObject<UMaterialExpressionComment>(Material.Get(), NAME_None, RF_Transactional);
		Expression->UpdateMaterialExpressionGuid(true, false);
		Material->GetExpressionCollection().AddComment(Expression);
		UMaterialGraphNode_Comment* Comment = Graph->AddComment(Expression, false);
		Comment->SetFlags(RF_Transactional);
		int32 L = MAX_int32, T = MAX_int32, R = MIN_int32, B = MIN_int32;
		for (UMaterialGraphNode* Node : Nodes)
		{
			L = FMath::Min(L, Node->NodePosX); T = FMath::Min(T, Node->NodePosY);
			R = FMath::Max(R, Node->NodePosX + Node->NodeWidth); B = FMath::Max(B, Node->NodePosY + Node->NodeHeight);
		}
		Comment->NodePosX = Expression->MaterialExpressionEditorX = L - 36;
		Comment->NodePosY = Expression->MaterialExpressionEditorY = T - 76;
		Comment->NodeWidth = Expression->SizeX = R - L + 72;
		Comment->NodeHeight = Expression->SizeY = B - T + 112;
		return Comment;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBPALMaterialChainTest, "BlueprintAutoLayout.Material.ConnectedChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBPALMaterialChainTest::RunTest(const FString& Parameters)
{
	FMaterialFixture F;
	auto* Source = F.Add<UMaterialExpressionConstant>(900, 500);
	auto* Declaration = F.Add<UMaterialExpressionNamedRerouteDeclaration>(700, 0);
	auto* Usage = F.Add<UMaterialExpressionNamedRerouteUsage>(400, 300);
	auto* Consumer = F.Add<UMaterialExpressionMultiply>(-300, 100);
	auto* Unrelated = F.Add<UMaterialExpressionMultiply>(2000, 2000);
	CastChecked<UMaterialExpressionNamedRerouteUsage>(Usage->MaterialExpression)->Declaration = CastChecked<UMaterialExpressionNamedRerouteDeclaration>(Declaration->MaterialExpression);
	Source->GetOutputPin(0)->MakeLinkTo(Declaration->GetInputPin(0));
	Source->GetOutputPin(0)->MakeLinkTo(Unrelated->GetInputPin(0));
	Usage->GetOutputPin(0)->MakeLinkTo(Consumer->GetInputPin(0));
	const TArray<UEdGraphNode*> Chain = BPALMaterialTools::CollectUpstream(F.Graph, { Consumer });
	TestEqual(TEXT("Includes implicit named-reroute dependency"), Chain.Num(), 4);
	TestFalse(TEXT("Excludes downstream fan-out"), Chain.Contains(Unrelated));
	BPALMaterialTools::Run(F.Graph, BPALMaterialTools::EAction::Chain, { Consumer });
	TestTrue(TEXT("Dependency chain ranks left to right"), Source->NodePosX < Declaration->NodePosX && Declaration->NodePosX < Usage->NodePosX && Usage->NodePosX < Consumer->NodePosX);
	TestEqual(TEXT("Unrelated branch stays put"), FIntPoint(Unrelated->NodePosX, Unrelated->NodePosY), FIntPoint(2000, 2000));
	TestTrue(TEXT("Repeated seeds do not duplicate dependencies"), BPALMaterialTools::CollectUpstream(F.Graph, {Consumer, Consumer}).Num() == 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBPALMaterialBlocksTest, "BlueprintAutoLayout.Material.GroupsAndPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBPALMaterialBlocksTest::RunTest(const FString& Parameters)
{
	FMaterialFixture F;
	auto* A = F.Add<UMaterialExpressionConstant>(900, 500);
	auto* B = F.Add<UMaterialExpressionMultiply>(1300, 500);
	auto* C = F.Add<UMaterialExpressionConstant>(-900, -500);
	auto* D = F.Add<UMaterialExpressionMultiply>(-500, -500);
	A->GetOutputPin(0)->MakeLinkTo(B->GetInputPin(0));
	C->GetOutputPin(0)->MakeLinkTo(D->GetInputPin(0));
	auto* First = F.Comment({A, B});
	auto* Second = F.Comment({C, D});
	BPALMaterialTools::Run(F.Graph, BPALMaterialTools::EAction::Graph);
	auto Box = [](UEdGraphNode* N) { return FBox2D(FVector2D(N->NodePosX, N->NodePosY), FVector2D(N->NodePosX + N->NodeWidth, N->NodePosY + N->NodeHeight)); };
	TestFalse(TEXT("Comment blocks do not overlap"), Box(First).Intersect(Box(Second)));
	TestTrue(TEXT("First group retains its nodes"), Box(First).IsInside(Box(A).GetCenter()) && Box(First).IsInside(Box(B).GetCenter()));
	const FIntPoint OldA(A->NodePosX, A->NodePosY), OldB(B->NodePosX, B->NodePosY);
	const FBox2D OldFirst = Box(First);
	BPALMaterialTools::SetPinned({First}, true, false);
	BPALMaterialTools::Run(F.Graph, BPALMaterialTools::EAction::Graph);
	TestEqual(TEXT("Pinned comment keeps its position and size"), Box(First), OldFirst);
	TestEqual(TEXT("Pinned comment freezes member A"), FIntPoint(A->NodePosX, A->NodePosY), OldA);
	TestEqual(TEXT("Pinned comment freezes member B"), FIntPoint(B->NodePosX, B->NodePosY), OldB);
	BPALMaterialTools::SetPinned({D}, true, false);
	const FIntPoint OldD(D->NodePosX, D->NodePosY);
	BPALMaterialTools::Run(F.Graph, BPALMaterialTools::EAction::Selected, {C, D});
	TestEqual(TEXT("Pinned individual node stays put in subset layout"), FIntPoint(D->NodePosX, D->NodePosY), OldD);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBPALMaterialOutputGroupsTest, "BlueprintAutoLayout.Material.OutputGrouping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBPALMaterialOutputGroupsTest::RunTest(const FString& Parameters)
{
	FMaterialFixture F;
	auto* Shared = F.Add<UMaterialExpressionConstant>(0, 0);
	auto* Color = F.Add<UMaterialExpressionMultiply>(500, 0);
	auto* Roughness = F.Add<UMaterialExpressionMultiply>(500, 300);
	Shared->GetOutputPin(0)->MakeLinkTo(Color->GetInputPin(0));
	Shared->GetOutputPin(0)->MakeLinkTo(Roughness->GetInputPin(0));
	Color->GetOutputPin(0)->MakeLinkTo(F.Graph->RootNode->GetInputPin(F.Graph->GetInputIndexForProperty(MP_BaseColor)));
	Roughness->GetOutputPin(0)->MakeLinkTo(F.Graph->RootNode->GetInputPin(F.Graph->GetInputIndexForProperty(MP_Roughness)));
	F.Graph->LinkMaterialExpressionsFromGraph();
	const int32 OriginalCount = F.Graph->Nodes.Num();
	BPALMaterialTools::Run(F.Graph, BPALMaterialTools::EAction::Group);
	TestEqual(TEXT("One comment per output plus shared dependencies"), F.Graph->Nodes.Num(), OriginalCount + 3);
	int32 SharedGroups = 0;
	TArray<FBox2D> Boxes;
	for (UEdGraphNode* Node : F.Graph->Nodes) if (auto* Comment = Cast<UMaterialGraphNode_Comment>(Node))
	{
		if (Comment->MaterialExpressionComment->Text == TEXT("Shared Dependencies")) ++SharedGroups;
		Boxes.Add(FBox2D(FVector2D(Node->NodePosX, Node->NodePosY), FVector2D(Node->NodePosX + Node->NodeWidth, Node->NodePosY + Node->NodeHeight)));
	}
	TestEqual(TEXT("Shared dependency group exists exactly once"), SharedGroups, 1);
	for (int32 I = 0; I < Boxes.Num(); ++I) for (int32 J = I + 1; J < Boxes.Num(); ++J) TestFalse(TEXT("Generated groups do not overlap"), Boxes[I].Intersect(Boxes[J]));
	TestTrue(TEXT("Grouping is one undo step"), GEditor->UndoTransaction());
	TestEqual(TEXT("Undo removes generated comments"), F.Graph->Nodes.Num(), OriginalCount);
	TestEqual(TEXT("Undo removes comments from material collection"), F.Material->GetExpressionCollection().EditorComments.Num(), 0);
	BPALMaterialTools::Run(F.Graph, BPALMaterialTools::EAction::Group);
	BPALMaterialTools::Run(F.Graph, BPALMaterialTools::EAction::Group);
	TestEqual(TEXT("Regrouping reuses comments"), F.Graph->Nodes.Num(), OriginalCount + 3);
	TestTrue(TEXT("Grouping preserves original wires"), Shared->GetOutputPin(0)->LinkedTo.Contains(Color->GetInputPin(0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBPALMaterialRoutingTest, "BlueprintAutoLayout.Material.NativeRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBPALMaterialRoutingTest::RunTest(const FString& Parameters)
{
	FMaterialFixture F;
	F.Graph->RootNode->NodePosX = 2000;
	F.Graph->RootNode->NodePosY = 600;
	auto* Source = F.Add<UMaterialExpressionVectorParameter>(0, 0);
	auto* Consumer = F.Add<UMaterialExpressionMultiply>(1100, 0);
	F.Add<UMaterialExpressionConstant>(500, 0);
	auto* UserReroute = F.Add<UMaterialExpressionReroute>(-700, -700);
	Source->GetOutputPin(2)->MakeLinkTo(Consumer->GetInputPin(0));
	F.Graph->LinkMaterialExpressionsFromGraph();
	auto* ConsumerExpression = CastChecked<UMaterialExpressionMultiply>(Consumer->MaterialExpression);
	const int32 OriginalOutput = ConsumerExpression->A.OutputIndex;
	const int32 OriginalCount = F.Graph->Nodes.Num();
	{
		const FScopedTransaction Transaction(FText::FromString(TEXT("Test material routing")));
		F.Graph->Modify(); F.Material->Modify();
		for (UEdGraphNode* Node : F.Graph->Nodes)
		{
			Node->Modify();
			if (auto* ExpressionNode = Cast<UMaterialGraphNode>(Node)) ExpressionNode->MaterialExpression->Modify();
		}
		BPALMaterialTools::RouteWires(F.Graph);
	}
	TestTrue(TEXT("Obstructed wire receives material reroutes"), F.Graph->Nodes.Num() > OriginalCount);
	TestTrue(TEXT("Saved consumer input references real reroute expression"), ConsumerExpression->A.Expression && ConsumerExpression->A.Expression->IsA<UMaterialExpressionReroute>());
	FExpressionInput* SavedInput = &ConsumerExpression->A;
	TSet<UMaterialExpression*> Seen;
	while (auto* Reroute = Cast<UMaterialExpressionReroute>(SavedInput->Expression))
	{
		if (Seen.Contains(Reroute)) break;
		Seen.Add(Reroute);
		SavedInput = &Reroute->Input;
	}
	TestEqual(TEXT("Saved reroute chain reaches the original source"), SavedInput->Expression, Source->MaterialExpression.Get());
	TestEqual(TEXT("Saved reroute chain preserves source channel"), SavedInput->OutputIndex, OriginalOutput);
	TestTrue(TEXT("Routing undo succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("Routing undo removes generated nodes"), F.Graph->Nodes.Num(), OriginalCount);
	TestEqual(TEXT("Routing undo restores expression input"), ConsumerExpression->A.Expression, Source->MaterialExpression.Get());
	TestEqual(TEXT("Routing undo preserves source channel"), ConsumerExpression->A.OutputIndex, OriginalOutput);
	BPALMaterialTools::RouteWires(F.Graph);
	const int32 RoutedCount = F.Graph->Nodes.Num();
	BPALMaterialTools::RemoveGeneratedRoutes(F.Graph);
	TestEqual(TEXT("Cleanup removes only generated reroutes"), F.Graph->Nodes.Num(), OriginalCount);
	TestTrue(TEXT("User reroute survives"), F.Graph->Nodes.Contains(UserReroute));
	TestEqual(TEXT("Cleanup restores original output channel"), ConsumerExpression->A.OutputIndex, OriginalOutput);
	TestEqual(TEXT("Cleanup reconnects saved expression input"), ConsumerExpression->A.Expression, Source->MaterialExpression.Get());
	BPALMaterialTools::RouteWires(F.Graph);
	TestEqual(TEXT("Rerouting does not accumulate nodes"), F.Graph->Nodes.Num(), RoutedCount);
	F.Graph->RebuildGraph();
	TestEqual(TEXT("Saved reroute expressions survive graph rebuild"), F.Graph->Nodes.Num(), RoutedCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBPALMaterialToolbarTest, "BlueprintAutoLayout.Material.EditorToolbar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBPALMaterialToolbarTest::RunTest(const FString& Parameters)
{
	FMaterialFixture F;
	auto* Source = F.Add<UMaterialExpressionConstant>(0, 0);
	auto* Consumer = F.Add<UMaterialExpressionMultiply>(500, 100);
	Source->GetOutputPin(0)->MakeLinkTo(Consumer->GetInputPin(0));
	Source->GetOutputPin(0)->MakeLinkTo(F.Graph->RootNode->GetInputPin(F.Graph->GetInputIndexForProperty(MP_Roughness)));
	Consumer->GetOutputPin(0)->MakeLinkTo(F.Graph->RootNode->GetInputPin(F.Graph->GetInputIndexForProperty(MP_BaseColor)));
	F.Graph->LinkMaterialExpressionsFromGraph();
	IMaterialEditorModule& Module = FModuleManager::LoadModuleChecked<IMaterialEditorModule>("MaterialEditor");
	TSharedRef<IMaterialEditor> Editor = Module.CreateMaterialEditor(EToolkitMode::Standalone, nullptr, F.Material.Get());
	UMaterialGraph* VisibleGraph = BPALMaterialTools::FindGraphForToolkit(Editor);
	TestNotNull(TEXT("Toolbar resolves the visible Material Editor graph"), VisibleGraph);
	if (VisibleGraph) TestTrue(TEXT("Toolbar operates on editor preview rather than the source asset"), VisibleGraph != F.Graph);
	UAssetEditorToolkitMenuContext* ToolkitContext = NewObject<UAssetEditorToolkitMenuContext>();
	ToolkitContext->Toolkit = Editor;
	FToolMenuContext Context;
	Context.AddObject(ToolkitContext);
	UToolMenu* Menu = UToolMenus::Get()->GenerateMenu("AssetEditor.MaterialEditor.ToolBar", Context);
	const FToolMenuSection* Section = Menu->FindSection("BlueprintAutoLayout");
	TestNotNull(TEXT("Material toolbar section generated"), Section);
	if (Section) TestNotNull(TEXT("Material toolbar entry generated for its toolkit"), Section->FindEntry("MaterialAutoLayout"));
	if (VisibleGraph)
	{
		TSharedPtr<SGraphEditor> GraphEditor = SGraphEditor::FindGraphEditorForGraph(VisibleGraph);
		UEdGraphNode* Selected = VisibleGraph->Nodes.Last();
		GraphEditor->ClearSelectionSet(); GraphEditor->SetNodeSelection(Selected, true);
		BPALMaterialTools::Run(VisibleGraph, BPALMaterialTools::EAction::Chain, {Selected});
		TestTrue(TEXT("Layout-and-frame preserves graph selection"), GraphEditor->GetSelectedNodes().Contains(Selected));
		BPALMaterialTools::Run(VisibleGraph, BPALMaterialTools::EAction::GroupAndRoute);
		TestTrue(TEXT("Grouping and native routing run in an open editor"), VisibleGraph->Nodes.Num() > 3);
		TestEqual(TEXT("Editor actions leave the source asset untouched until Apply"), F.Graph->Nodes.Num(), 3);
	}
	TStrongObjectPtr<UMaterialFunction> Function(NewObject<UMaterialFunction>());
	auto* FunctionSource = NewObject<UMaterialExpressionConstant>(Function.Get());
	auto* Output = NewObject<UMaterialExpressionFunctionOutput>(Function.Get());
	Output->OutputName = "Result";
	Output->A.Connect(0, FunctionSource);
	Function->GetExpressionCollection().AddExpression(FunctionSource);
	Function->GetExpressionCollection().AddExpression(Output);
	TSharedRef<IMaterialEditor> FunctionEditor = Module.CreateMaterialEditor(EToolkitMode::Standalone, nullptr, Function.Get());
	UMaterialGraph* FunctionGraph = BPALMaterialTools::FindGraphForToolkit(FunctionEditor);
	TestNotNull(TEXT("Toolbar resolves a material function graph"), FunctionGraph);
	TestEqual(TEXT("Another active editor cannot redirect the original toolbar"), BPALMaterialTools::FindGraphForToolkit(Editor), VisibleGraph);
	FunctionEditor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted);
	Editor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted);
	return true;
}
#endif
