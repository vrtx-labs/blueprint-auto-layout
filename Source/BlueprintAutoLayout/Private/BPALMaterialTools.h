// Copyright (c) 2026 Alex Coulombe. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"

#define BPAL_MATERIAL_TOOLS (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8))

class UMaterialGraph;
class UEdGraphNode;
class UToolMenus;
class FAssetEditorToolkit;
struct FToolMenuSection;

namespace BPALMaterialTools
{
#if BPAL_MATERIAL_TOOLS
	enum class EAction { Graph, Selected, Chain, Group, Route, GroupAndRoute };
	TArray<UEdGraphNode*> CollectUpstream(UMaterialGraph* Graph, const TArray<UEdGraphNode*>& Seeds);
	void GatherDependencies(UEdGraphNode* Node, TArray<UEdGraphNode*>& Dependencies);
	bool IsPinned(UEdGraphNode* Node);
	void SetPinned(const TArray<UEdGraphNode*>& Nodes, bool bPinned, bool bSave = true);
	void GroupOutputs(UMaterialGraph* Graph);
	void RemoveGeneratedRoutes(UMaterialGraph* Graph);
	void RouteWires(UMaterialGraph* Graph);
	void Run(UMaterialGraph* Graph, EAction Action, const TArray<UEdGraphNode*>& Seeds = {});
	void AddContextEntries(FToolMenuSection& Section, UMaterialGraph* Graph, UEdGraphNode* ContextNode);
	void RegisterToolbar(UToolMenus* Menus);
	UMaterialGraph* FindGraphForToolkit(TWeakPtr<FAssetEditorToolkit> Toolkit);
	void TrackFocusedGraph();
	void RefreshOpenEditor(UMaterialGraph* Graph);
#endif
}
