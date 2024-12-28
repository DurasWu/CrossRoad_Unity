// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "AllegroComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AllegroComponent)

#define LOCTEXT_NAMESPACE "Allegro"

void UAllegroComponent::K2_GetInstanceCustomStruct(EAllegroValidity& ExecResult, int InstanceIndex, int32& InStruct)
{
	checkNoEntry();
}

void UAllegroComponent::K2_SetInstanceCustomStruct(int InstanceIndex, const int32& InStruct)
{
	checkNoEntry();
}

DEFINE_FUNCTION(UAllegroComponent::execK2_GetInstanceCustomStruct)
{
	UAllegroComponent* AllegroComponent = P_THIS;
	P_GET_ENUM_REF(EAllegroValidity, ExecResult);
	P_GET_PROPERTY(FIntProperty, InstanceIndex);

	// Read wildcard Value input.
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentPropertyContainer = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);

	const FStructProperty* ValueProp = CastField<FStructProperty>(Stack.MostRecentProperty);
	void* ValuePtr = Stack.MostRecentPropertyAddress;

	P_FINISH;

	ExecResult = EAllegroValidity::NotValid;

	if (!ValueProp || !ValuePtr)
	{
		FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, FBlueprintExceptionInfo(EBlueprintExceptionType::AbortExecution, INVTEXT("Failed to resolve the Value")));
		return;
	}

	if (!::IsValid(AllegroComponent))
	{
		FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, FBlueprintExceptionInfo(EBlueprintExceptionType::AccessViolation, INVTEXT("Invalid Component")));
		return;
	}

	if (!AllegroComponent->IsInstanceValid(InstanceIndex))
	{
		//FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, FBlueprintExceptionInfo(EBlueprintExceptionType::AccessViolation, INVTEXT("Invalid InstanceIndex")));
		return;
	}

	const UScriptStruct* ScriptStruct = AllegroComponent->PerInstanceScriptStruct;
	const bool bCompatible = ScriptStruct == ValueProp->Struct || (ScriptStruct && ScriptStruct->IsChildOf(ValueProp->Struct));
	if (!bCompatible)
	{
		//FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, FBlueprintExceptionInfo(EBlueprintExceptionType::AccessViolation, INVTEXT("CustomPerInstanceStruct Mismatch")));
		return;
	}

	{
		P_NATIVE_BEGIN;
		const int StructSize = ScriptStruct->GetStructureSize();
		uint8* InstanceStructPtr = AllegroComponent->InstancesData.CustomPerInstanceStruct.GetData() + (StructSize * InstanceIndex);
		ValueProp->Struct->CopyScriptStruct(ValuePtr, InstanceStructPtr);
		ExecResult = EAllegroValidity::Valid;
		P_NATIVE_END;
	}
}

DEFINE_FUNCTION(UAllegroComponent::execK2_SetInstanceCustomStruct)
{
	UAllegroComponent* AllegroComponent = P_THIS;
	P_GET_PROPERTY(FIntProperty, InstanceIndex);

	// Read wildcard Value input.
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentPropertyContainer = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);

	const FStructProperty* ValueProp = CastField<FStructProperty>(Stack.MostRecentProperty);
	void* ValuePtr = Stack.MostRecentPropertyAddress;

	P_FINISH;

	if (!ValueProp || !ValuePtr)
	{
		FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, FBlueprintExceptionInfo(EBlueprintExceptionType::AbortExecution, INVTEXT("Failed to resolve the Value")));
		return;
	}

	if (!::IsValid(AllegroComponent))
	{
		FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, FBlueprintExceptionInfo(EBlueprintExceptionType::AccessViolation, INVTEXT("Invalid Component")));
		return;
	}

	if (!AllegroComponent->IsInstanceValid(InstanceIndex))
	{
		FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, FBlueprintExceptionInfo(EBlueprintExceptionType::AccessViolation, INVTEXT("Invalid InstanceIndex")));
		return;
	}

	const UScriptStruct* ScriptStruct = AllegroComponent->PerInstanceScriptStruct;
	const bool bCompatible = ScriptStruct == ValueProp->Struct || (ScriptStruct && ValueProp->Struct->IsChildOf(ScriptStruct));
	if (!bCompatible)
	{
		FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, FBlueprintExceptionInfo(EBlueprintExceptionType::AccessViolation, INVTEXT("Incompatible Struct")));
		return;
	}

	{
		P_NATIVE_BEGIN;
		const int StructSize = ScriptStruct->GetStructureSize();
		uint8* InstanceStructPtr = AllegroComponent->InstancesData.CustomPerInstanceStruct.GetData() + (StructSize * InstanceIndex);
		ValueProp->Struct->CopyScriptStruct(InstanceStructPtr, ValuePtr);
		P_NATIVE_END;
	}
}

#undef LOCTEXT_NAMESPACE