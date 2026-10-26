// Copyright (c) 2026 Alex Coulombe. Licensed under the MIT License.
#include "BPALMaterialTools.h"

#if BPAL_MATERIAL_TOOLS
#include "BlueprintAutoLayoutSettings.h"
#include "MaterialGraph/MaterialGraph.h"
#include "GraphEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/AssetEditorToolkitMenuContext.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Docking/SDockTab.h"
#include "Styling/AppStyle.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"
#include "IMaterialEditor.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "BPALMaterialEditor"
namespace BPALMaterialTools
{
static TWeakObjectPtr<UMaterialGraph> LastFocusedGraph;
static TArray<TWeakPtr<FAssetEditorToolkit>> OpenToolkits;

void TrackFocusedGraph()
{
	if (!FSlateApplication::IsInitialized()) return;
	for (TSharedPtr<SWidget> Widget = FSlateApplication::Get().GetKeyboardFocusedWidget(); Widget; Widget = Widget->GetParentWidget())
	{
		if (Widget->GetType() != "SGraphEditor" && Widget->GetType() != "SGraphEditorImpl") continue;
		if (UMaterialGraph* Graph = Cast<UMaterialGraph>(StaticCastSharedPtr<SGraphEditor>(Widget)->GetCurrentGraph())) LastFocusedGraph = Graph;
		break;
	}
}

void RefreshOpenEditor(UMaterialGraph* Graph)
{
	OpenToolkits.RemoveAll([](const TWeakPtr<FAssetEditorToolkit>& Weak) { return !Weak.IsValid(); });
	for (const TWeakPtr<FAssetEditorToolkit>& Weak : OpenToolkits)
	{
		TSharedPtr<IMaterialEditor> Editor = StaticCastSharedPtr<IMaterialEditor>(Weak.Pin());
		if (Editor && Editor->GetMaterialInterface() == Graph->Material)
		{
			Editor->UpdateMaterialAfterGraphChange();
			break;
		}
	}
}

static TArray<UEdGraphNode*> SelectionFor(UMaterialGraph* Graph, UEdGraphNode* ContextNode = nullptr)
{
	TArray<UEdGraphNode*> Nodes;
	if (TSharedPtr<SGraphEditor> Editor = SGraphEditor::FindGraphEditorForGraph(Graph))
		for (UObject* Object : Editor->GetSelectedNodes()) if (UEdGraphNode* Node = Cast<UEdGraphNode>(Object)) Nodes.Add(Node);
	if (ContextNode && !Nodes.Contains(ContextNode)) Nodes = { ContextNode };
	return Nodes;
}

struct FEntry
{
	FName Name;
	FText Label;
	FText Tooltip;
	FUIAction Action;
};

static TArray<FEntry> EntriesFor(UMaterialGraph* Graph, const TArray<UEdGraphNode*>& Nodes)
{
	TWeakObjectPtr<UMaterialGraph> WeakGraph(Graph);
	TArray<TWeakObjectPtr<UEdGraphNode>> WeakNodes;
	for (UEdGraphNode* Node : Nodes) WeakNodes.Add(Node);
	auto ResolveNodes = [WeakNodes]() {
		TArray<UEdGraphNode*> Result;
		for (const auto& Weak : WeakNodes) if (UEdGraphNode* Node = Weak.Get()) Result.Add(Node);
		return Result;
	};
	TArray<FEntry> Entries;
	auto Add = [&](FName Name, FText Label, FText Tooltip, EAction Action, bool bNeedsSelection = false) {
		Entries.Add({Name, Label, Tooltip, FUIAction(
			FExecuteAction::CreateLambda([WeakGraph, ResolveNodes, Action]() { if (UMaterialGraph* G = WeakGraph.Get()) Run(G, Action, ResolveNodes()); }),
			FCanExecuteAction::CreateLambda([WeakGraph, ResolveNodes, bNeedsSelection]() { return WeakGraph.IsValid() && (!bNeedsSelection || !ResolveNodes().IsEmpty()); }))});
	};
	Add("MaterialLayoutGraph", LOCTEXT("Graph", "Auto Layout Graph"), LOCTEXT("GraphTip", "Arrange this material graph. Pinned nodes and existing groups are respected."), EAction::Graph);
	Add("MaterialLayoutSelected", LOCTEXT("Selected", "Auto Layout Selected"), LOCTEXT("SelectedTip", "Arrange selected nodes without moving unselected expressions."), EAction::Selected, true);
	Add("MaterialLayoutChain", LOCTEXT("Chain", "Layout Connected Chain"), LOCTEXT("ChainTip", "Arrange selected nodes and all upstream dependencies, including named reroute declarations."), EAction::Chain, true);
	Add("MaterialLayoutGroup", LOCTEXT("Group", "Auto Layout & Group by Output"), LOCTEXT("GroupTip", "Group material outputs and shared dependencies into named, colored comments."), EAction::Group);
	Add("MaterialLayoutRoute", LOCTEXT("Route", "Auto Layout & Route Wires"), LOCTEXT("RouteTip", "Arrange the graph and insert material reroutes on long or obstructed wires."), EAction::Route);
	Add("MaterialLayoutGroupRoute", LOCTEXT("GroupRoute", "Auto Layout, Group & Route"), LOCTEXT("GroupRouteTip", "Arrange, group by output, and route material wires in one undo step."), EAction::GroupAndRoute);
	for (bool bPin : {true, false})
	{
		Entries.Add({bPin ? FName("MaterialPin") : FName("MaterialUnpin"),
			bPin ? LOCTEXT("Pin", "Pin Selected for Layout") : LOCTEXT("Unpin", "Unpin Selected for Layout"),
			LOCTEXT("PinTip", "A pinned node stays in place. Pinning a comment locks its bounds and all enclosed nodes. Stored in per-project user preferences."),
			FUIAction(FExecuteAction::CreateLambda([ResolveNodes, bPin]() { SetPinned(ResolveNodes(), bPin); }),
				FCanExecuteAction::CreateLambda([ResolveNodes, bPin]() {
					for (UEdGraphNode* Node : ResolveNodes()) if (IsPinned(Node) != bPin) return true;
					return false;
				}))});
	}
	return Entries;
}

void AddContextEntries(FToolMenuSection& Section, UMaterialGraph* Graph, UEdGraphNode* ContextNode)
{
	for (const FEntry& Entry : EntriesFor(Graph, SelectionFor(Graph, ContextNode)))
		Section.AddMenuEntry(Entry.Name, Entry.Label, Entry.Tooltip, FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.AlignNodesTop"), Entry.Action);
}

static TSharedPtr<SGraphEditor> FindGraphWidget(const TSharedRef<SWidget>& Widget, UMaterialInterface* Preview)
{
	if (!Widget->GetVisibility().IsVisible()) return nullptr;
	const FName Type = Widget->GetType();
	if (Type == "SGraphEditor" || Type == "SGraphEditorImpl")
	{
		TSharedRef<SGraphEditor> Editor = StaticCastSharedRef<SGraphEditor>(Widget);
		if (UMaterialGraph* Graph = Cast<UMaterialGraph>(Editor->GetCurrentGraph()))
			if (Graph->Material == Preview) return Editor;
	}
	FChildren* Children = Widget->GetChildren();
	for (int32 I = 0; Children && I < Children->Num(); ++I)
		if (TSharedPtr<SGraphEditor> Editor = FindGraphWidget(Children->GetChildAt(I), Preview)) return Editor;
	return nullptr;
}

UMaterialGraph* FindGraphForToolkit(TWeakPtr<FAssetEditorToolkit> WeakToolkit)
{
	const TSharedPtr<FAssetEditorToolkit> Toolkit = WeakToolkit.Pin();
	if (!Toolkit || !Toolkit->GetTabManager()) return nullptr;
	UMaterialInterface* Preview = StaticCastSharedPtr<IMaterialEditor>(Toolkit)->GetMaterialInterface();
	TrackFocusedGraph();
	// Active graph tabs may be floated outside the major editor tab.
	const TSharedPtr<SDockTab> Active = FGlobalTabmanager::Get()->GetActiveTab();
	if (Active && Active->GetTabManagerPtr() == Toolkit->GetTabManager())
		if (TSharedPtr<SGraphEditor> Editor = FindGraphWidget(Active->GetContent(), Preview)) return Cast<UMaterialGraph>(Editor->GetCurrentGraph());
	if (LastFocusedGraph.IsValid() && LastFocusedGraph->Material == Preview && SGraphEditor::FindGraphEditorForGraph(LastFocusedGraph.Get())) return LastFocusedGraph.Get();
	if (TSharedPtr<SDockTab> Owner = Toolkit->GetTabManager()->GetOwnerTab())
		if (TSharedPtr<SGraphEditor> Editor = FindGraphWidget(Owner->GetContent(), Preview)) return Cast<UMaterialGraph>(Editor->GetCurrentGraph());
	for (const auto& Window : FSlateApplication::Get().GetTopLevelWindows())
		if (TSharedPtr<SGraphEditor> Editor = FindGraphWidget(Window, Preview)) return Cast<UMaterialGraph>(Editor->GetCurrentGraph());
	return nullptr;
}

void RegisterToolbar(UToolMenus* Menus)
{
	UToolMenu* Toolbar = Menus->ExtendMenu("AssetEditor.MaterialEditor.ToolBar");
	Toolbar->FindOrAddSection("BlueprintAutoLayout").AddDynamicEntry("MaterialAutoLayout",
		FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& Section) {
			UAssetEditorToolkitMenuContext* Context = Section.FindContext<UAssetEditorToolkitMenuContext>();
			if (!Context || !Context->Toolkit.IsValid()) return;
			const TWeakPtr<FAssetEditorToolkit> Toolkit = Context->Toolkit;
			OpenToolkits.AddUnique(Toolkit);
			Section.AddEntry(FToolMenuEntry::InitComboButton("MaterialAutoLayout",
				FUIAction(FExecuteAction::CreateLambda([Toolkit]() { Run(FindGraphForToolkit(Toolkit), EAction::Graph); }),
					FCanExecuteAction::CreateLambda([Toolkit]() { return FindGraphForToolkit(Toolkit) != nullptr; })),
				FOnGetContent::CreateLambda([Toolkit]() -> TSharedRef<SWidget> {
					UMaterialGraph* Graph = FindGraphForToolkit(Toolkit);
					FMenuBuilder Menu(true, nullptr);
					if (Graph) for (const FEntry& Entry : EntriesFor(Graph, SelectionFor(Graph))) Menu.AddMenuEntry(Entry.Label, Entry.Tooltip, FSlateIcon(), Entry.Action);
					Menu.AddMenuSeparator();
					Menu.AddMenuEntry(LOCTEXT("Frame", "Frame Nodes After Layout"), LOCTEXT("FrameTip", "Automatically zoom and pan to the nodes affected by layout."), FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([]() {
							UBlueprintAutoLayoutSettings* Settings = GetMutableDefault<UBlueprintAutoLayoutSettings>();
							Settings->bFrameMaterialLayout = !Settings->bFrameMaterialLayout; Settings->SaveConfig();
						}), FCanExecuteAction(), FIsActionChecked::CreateLambda([]() { return GetDefault<UBlueprintAutoLayoutSettings>()->bFrameMaterialLayout; })), NAME_None, EUserInterfaceActionType::ToggleButton);
					return Menu.MakeWidget();
				}), LOCTEXT("Toolbar", "Auto Layout"), LOCTEXT("ToolbarTip", "Arrange the visible material graph. Use the dropdown for selection, connected chain, grouping, routing and pinning."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.AlignNodesTop")));
		}));
}
}
#undef LOCTEXT_NAMESPACE
#endif
