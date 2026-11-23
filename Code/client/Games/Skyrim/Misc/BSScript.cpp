#include <Misc/BSScript.h>
#include <Misc/GameVM.h>

#include <algorithm>

uint64_t BSScript::Object::GetHandle()
{
    TP_THIS_FUNCTION(TGetHandle, uint64_t, BSScript::Object);
    POINTER_SKYRIMSE(TGetHandle, s_getHandle, 104247, 97463);
    return TiltedPhoques::ThisCall(s_getHandle, this);
}

BSScript::Variable::Variable()
{
}

BSScript::Variable::~Variable() noexcept
{
    Reset();
}

void BSScript::Variable::Reset() noexcept
{
    TP_THIS_FUNCTION(TReset, void, BSScript::Variable);

    POINTER_SKYRIMSE(TReset, s_reset, 104296, 104296);

    TiltedPhoques::ThisCall(s_reset, this);
}

void BSScript::Variable::Clear() noexcept
{
    Reset();

    type = kEmpty;
}

template <> void BSScript::Variable::Set(int32_t aValue) noexcept
{
    Reset();

    type = kInteger;
    data.i = aValue;
}

template <> void BSScript::Variable::Set(float aValue) noexcept
{
    Reset();

    type = kFloat;
    data.f = aValue;
}

template <> void BSScript::Variable::Set(bool aValue) noexcept
{
    Reset();

    type = kBoolean;
    data.b = aValue;
}

template <> void BSScript::Variable::Set(const char* acpValue) noexcept
{
    Reset();

    type = kString;
    auto pStr = (BSFixedString*)&data.s;
    pStr->Set(acpValue);
}

BSScript::Object* BSScript::Variable::GetObject() const noexcept
{
    if (type > Type::kMax)
    {
        bool isArray = type & 1;
        if (isArray)
            return nullptr;
    }
    else if (type != Type::kObject)
        return nullptr;

    return data.pObj;
}

template <class T> T* BSScript::Variable::ExtractComplexType() noexcept
{
    auto* pPolicy = GameVM::Get()->virtualMachine->GetObjectHandlePolicy();
    BSScript::Object* pBaseObject = GetObject();

    if (!pBaseObject || !pPolicy)
        return nullptr;

    uint64_t handle = pBaseObject->GetHandle();

    if (!pPolicy->HandleIsType((uint32_t)T::Type, handle) || !pPolicy->IsHandleObjectAvailable(handle))
        return nullptr;

    return pPolicy->GetObjectForHandle<T>(handle);
}

void BSScript::IFunctionArguments::Statement::SetSize(uint32_t aCount) noexcept
{
    TP_THIS_FUNCTION(TSetSize, void, BSScript::IFunctionArguments::Statement, uint32_t aCount);
    POINTER_SKYRIMSE(TSetSize, s_setSize, 53915, 53915);
    TiltedPhoques::ThisCall(s_setSize, this, aCount);
}

BSScript::IObjectHandlePolicy* BSScript::IObjectHandlePolicy::Get() noexcept
{
#ifndef SKYRIMVR
    POINTER_SKYRIMSE(BSScript::IObjectHandlePolicy*, s_policy, 414391, 414391);
    return *s_policy.Get();
#else
    // No verified VR address for 414391: ask the VM instead.
    auto* pVM = GameVM::Get();
    return pVM && pVM->virtualMachine ? pVM->virtualMachine->GetObjectHandlePolicy() : nullptr;
#endif
}

BSFixedString& BSScript::NativeFunctionBase::GetName()
{
    return name;
}

BSFixedString& BSScript::NativeFunctionBase::GetObjectTypeName()
{
    return objectName;
}

BSFixedString& BSScript::NativeFunctionBase::GetStateName()
{
    return stateName;
}

BSScript::Variable::Type& BSScript::NativeFunctionBase::GetReturnType()
{
    return returnType;
}

uint32_t BSScript::NativeFunctionBase::GetParamCount()
{
    return parameters.capacity;
}

void BSScript::NativeFunctionBase::GetParam(uint32_t aIndex, BSFixedString& apNameOut, Variable::Type& aTypeOut)
{
    if (aIndex < parameters.size)
    {
        auto& elem = parameters.data[aIndex];
        apNameOut = elem.name;
        aTypeOut = elem.type;
    }
    else
    {
        apNameOut = "";
        aTypeOut = Variable::Type::kEmpty;
    }
}

uint32_t BSScript::NativeFunctionBase::GetStackFrameSize()
{
    return parameters.capacity;
}

bool BSScript::NativeFunctionBase::GetIsNative()
{
    return true;
}

bool BSScript::NativeFunctionBase::GetIsStatic()
{
    return isStatic;
}

bool BSScript::NativeFunctionBase::GetIsEmpty()
{
    return false;
}

auto BSScript::NativeFunctionBase::GetFunctionType() -> FunctionType
{
    return FunctionType::kNormal;
}

uint32_t BSScript::NativeFunctionBase::GetUserFlags()
{
    return flags;
}

BSFixedString& BSScript::NativeFunctionBase::GetDocString()
{
    return documentation;
}

void BSScript::NativeFunctionBase::InsertLocals(StackFrame*)
{
    return;
}

BSScript::CallResult BSScript::NativeFunctionBase::Call(Stack* apStack, void* apLogger, IVirtualMachine* apVm, bool aArg4)
{
    TP_THIS_FUNCTION(TCall, CallResult, NativeFunctionBase, Stack*, void*, IVirtualMachine*, bool);
    POINTER_SKYRIMSE(TCall, s_call, 104651, 97923);
    return TiltedPhoques::ThisCall(s_call, this, apStack, apLogger, apVm, aArg4);
}

BSFixedString& BSScript::NativeFunctionBase::GetSourceFilename()
{
    static BSFixedString native("<native>");
    return native;
}

bool BSScript::NativeFunctionBase::TranslateIPToLineNumber(uint32_t, uint32_t& aLineNumberOut)
{
    aLineNumberOut = 0;
    return false;
}

bool BSScript::NativeFunctionBase::GetVarNameForStackIndex(std::uint32_t aIndex, BSFixedString& aNameOut)
{
    if (aIndex < parameters.capacity)
    {
        aNameOut = parameters.data[aIndex].name;
        return true;
    }
    else
    {
        aNameOut = "";
        return false;
    }
}

bool BSScript::NativeFunctionBase::CanBeCalledFromTasklets()
{
    return isCallableFromTask;
}

void BSScript::NativeFunctionBase::SetCallableFromTasklets(bool aCallable)
{
    isCallableFromTask = aCallable;
}

BSScript::IsRemotePlayerFunc::IsRemotePlayerFunc(const char* apFunctionName, const char* apClassName, FunctionType aFunction, Variable::Type aType)
    : NativeFunction(apFunctionName, apClassName, true, 1)
{
    pFunction = reinterpret_cast<void*>(aFunction);

    returnType = aType;

    BSFixedString arg1Name("Actor");
    void* ptr = nullptr;
    GameVM::Get()->virtualMachine->GetScriptObjectType1(&arg1Name, &ptr);
    parameters.data[0].pType = ptr;
}

bool BSScript::IsRemotePlayerFunc::MarshallAndDispatch(Variable* apBaseVar, IVirtualMachine* apVm, uint32_t aStackID, Variable* apResult, StackFrame* apStackFrame)
{
    uint32_t page = apStackFrame->GetPageForFrame();

    BSScript::Variable* pArg1 = apStackFrame->GetStackFrameVariable(0, page);
    Actor* pActor = pArg1->ExtractComplexType<Actor>();
    if (!pActor)
        return false;

    bool result = ((FunctionType*)pFunction)(pActor);

    apResult->Set<bool>(result);

    return true;
}

BSScript::IsPlayerFunc::IsPlayerFunc(const char* apFunctionName, const char* apClassName, FunctionType aFunction, Variable::Type aType)
    : NativeFunction(apFunctionName, apClassName, true, 1)
{
    pFunction = reinterpret_cast<void*>(aFunction);

    returnType = aType;

    BSFixedString arg1Name("Actor");
    void* ptr = nullptr;
    GameVM::Get()->virtualMachine->GetScriptObjectType1(&arg1Name, &ptr);
    parameters.data[0].pType = ptr;
}

bool BSScript::IsPlayerFunc::MarshallAndDispatch(Variable* apBaseVar, IVirtualMachine* apVm, uint32_t aStackID, Variable* apResult, StackFrame* apStackFrame)
{
    uint32_t page = apStackFrame->GetPageForFrame();

    BSScript::Variable* pArg1 = apStackFrame->GetStackFrameVariable(0, page);
    Actor* pActor = pArg1->ExtractComplexType<Actor>();
    if (!pActor)
        return false;

    bool result = ((FunctionType*)pFunction)(pActor);

    apResult->Set<bool>(result);

    return true;
}

BSScript::DidLaunchSkyrimTogetherFunc::DidLaunchSkyrimTogetherFunc(const char* apFunctionName, const char* apClassName, FunctionType aFunction, Variable::Type aType)
    : NativeFunction(apFunctionName, apClassName, true, 0)
{
    pFunction = reinterpret_cast<void*>(aFunction);
    returnType = aType;
}

bool BSScript::DidLaunchSkyrimTogetherFunc::MarshallAndDispatch(Variable* apBaseVar, IVirtualMachine* apVm, uint32_t aStackID, Variable* apResult, StackFrame* apStackFrame)
{
    apResult->Set<bool>(true);
    return true;
}

int32_t BSScript::NativeArguments::Int(uint32_t aIndex) const noexcept
{
    return aIndex < count && pVariables[aIndex] && pVariables[aIndex]->type == Variable::kInteger ? pVariables[aIndex]->data.i : 0;
}

float BSScript::NativeArguments::Float(uint32_t aIndex) const noexcept
{
    return aIndex < count && pVariables[aIndex] && pVariables[aIndex]->type == Variable::kFloat ? pVariables[aIndex]->data.f : 0.f;
}

bool BSScript::NativeArguments::Bool(uint32_t aIndex) const noexcept
{
    return aIndex < count && pVariables[aIndex] && pVariables[aIndex]->type == Variable::kBoolean && pVariables[aIndex]->data.b;
}

const char* BSScript::NativeArguments::String(uint32_t aIndex) const noexcept
{
    // A string variable holds a BSFixedString, whose first member is the pooled characters.
    const char* pText = aIndex < count && pVariables[aIndex] && pVariables[aIndex]->type == Variable::kString ? pVariables[aIndex]->data.s : nullptr;
    return pText ? pText : "";
}

void BSScript::NativeReturn::SetBool(bool aValue) const noexcept
{
    pResult->Set<bool>(aValue);
}

void BSScript::NativeReturn::SetInt(int32_t aValue) const noexcept
{
    pResult->Set<int32_t>(aValue);
}

void BSScript::NativeReturn::SetString(const char* acpValue) const noexcept
{
    pResult->Set<const char*>(acpValue ? acpValue : "");
}

BSScript::GlobalNativeFunc::GlobalNativeFunc(
    const char* apFunctionName, const char* apClassName, Handler* apHandler, Variable::Type aReturnType, std::initializer_list<Variable::Type> aParameterTypes)
    : NativeFunction(apFunctionName, apClassName, true, static_cast<uint32_t>(std::min<size_t>(aParameterTypes.size(), kMostParameters)))
    , m_parameterCount(static_cast<uint32_t>(std::min<size_t>(aParameterTypes.size(), kMostParameters)))
{
    pFunction = reinterpret_cast<void*>(apHandler);
    returnType = aReturnType;

    // Only the type is set: the game's constructor already made the parameter entries, as for the functions above.
    uint32_t index = 0;
    for (const Variable::Type type : aParameterTypes)
    {
        if (index >= m_parameterCount)
            break;
        parameters.data[index++].type = type;
    }
}

bool BSScript::GlobalNativeFunc::MarshallAndDispatch(Variable*, IVirtualMachine*, uint32_t, Variable* apResult, StackFrame* apStackFrame)
{
    Variable* variables[kMostParameters]{};
    if (m_parameterCount)
    {
        const uint32_t page = apStackFrame->GetPageForFrame();
        for (uint32_t i = 0; i < m_parameterCount; ++i)
            variables[i] = apStackFrame->GetStackFrameVariable(i, page);
    }

    const NativeArguments arguments{variables, m_parameterCount};
    const NativeReturn result{apResult};
    // An exception must not cross into the game's VM; the script then gets the default value.
    try
    {
        reinterpret_cast<Handler*>(pFunction)(arguments, result);
    }
    catch (...)
    {
    }
    return true;
}
