// 在 EngineNative.cpp 的会话检查与异常边界内导出对象图操作。
int32_t FLARE_CALL flare_logic_operation(FLARE_Session handle,uint32_t operation,uint64_t object,
                                  uint64_t target,double value,uint64_t* outToken,double* outValue) noexcept
{
    return Protect([&]() -> int32_t {
        auto* session=Find(handle);
        if(!session)return MissingSession();
        if(!outToken||!outValue)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        auto& world=session->WorldRef();
        if(operation==6&&(session->stepping||world.IsInFrame()||world.IsIterating()||world.HasPendingCommands()))
            return Error(FLARE_BUSY,"Bridge.SessionBusy");
        if(!session->logicObjects.Execute(world.Objects(),operation,object,target,value,*outToken,*outValue))
            return Error(FLARE_INVALID_HANDLE,"Bridge.InvalidHandle");
        return FLARE_OK;
    });
}
