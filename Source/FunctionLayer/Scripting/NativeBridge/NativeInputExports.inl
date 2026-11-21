int32_t SE_CALL se_input_load_actions(SE_Session handle, const char* json_utf8, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!json_utf8 || length == 0 || length > 1024u * 1024u || !ValidUtf8(json_utf8, length))
        {
            return Error(SE_INVALID_ARGUMENT, "Input.InvalidDocument");
        }
        se::InputActionDocument parsed;
        std::string errorKey;
        if (!se::ParseInputActionDocument(std::string_view(json_utf8, length), parsed, errorKey))
        {
            return Error(SE_INVALID_ARGUMENT, errorKey.empty() ? "Input.InvalidDocument" : errorKey.c_str());
        }
        s->inputActions.SetDocument(std::move(parsed));
        return SE_OK;
    });
}

int32_t SE_CALL se_input_action_button(
    SE_Session handle, const char* action, uint32_t length, int32_t* down, int32_t* pressed) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!action || length == 0 || length > 128u || !ValidUtf8(action, length) || (!down && !pressed))
        {
            return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        }
        const std::string_view name(action, length);
        if (down) *down = s->inputActions.IsActionDown(name) ? 1 : 0;
        if (pressed) *pressed = s->inputActions.IsActionPressed(name) ? 1 : 0;
        return SE_OK;
    });
}

int32_t SE_CALL se_input_action_axis1d(
    SE_Session handle, const char* action, uint32_t length, float* value) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!action || length == 0 || length > 128u || !ValidUtf8(action, length) || !value)
        {
            return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        }
        *value = s->inputActions.GetAxis1D(std::string_view(action, length));
        return SE_OK;
    });
}

int32_t SE_CALL se_input_action_axis2d(
    SE_Session handle, const char* action, uint32_t length, SE_Vector2* value) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!action || length == 0 || length > 128u || !ValidUtf8(action, length) || !value)
        {
            return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        }
        const se::Vec2 v = s->inputActions.GetAxis2D(std::string_view(action, length));
        *value = {v.x, v.y};
        return SE_OK;
    });
}

int32_t SE_CALL se_input_get_mouse_delta(SE_Session handle, SE_Vector2* delta) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!delta) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *delta = s->mouseDelta;
        return SE_OK;
    });
}

int32_t SE_CALL se_input_set_cursor_mode(SE_Session handle, int32_t locked, int32_t visible) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        const bool nextLocked = locked != 0;
        const bool nextVisible = visible != 0;
        if (!nextLocked && s->cursorLocked)
            ReleaseSessionCursor(*s);
        s->cursorLocked = nextLocked;
        s->cursorVisible = nextVisible;
        if (nextLocked)
            s->cursorLockPausedByEsc = false;
        if (!nextLocked && nextVisible)
            ReleaseSessionCursor(*s);
        return SE_OK;
    });
}

int32_t SE_CALL se_input_get_cursor_mode(SE_Session handle, int32_t* locked, int32_t* visible) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (locked) *locked = (s->cursorLocked && !s->cursorLockPausedByEsc) ? 1 : 0;
        if (visible) *visible = s->cursorVisible ? 1 : 0;
        return SE_OK;
    });
}
