int32_t FLARE_CALL flare_input_load_actions(FLARE_Session handle, const char* json_utf8, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!json_utf8 || length == 0 || length > 1024u * 1024u || !ValidUtf8(json_utf8, length))
        {
            return Error(FLARE_INVALID_ARGUMENT, "Input.InvalidDocument");
        }
        flare::InputActionDocument parsed;
        std::string errorKey;
        if (!flare::ParseInputActionDocument(std::string_view(json_utf8, length), parsed, errorKey))
        {
            return Error(FLARE_INVALID_ARGUMENT, errorKey.empty() ? "Input.InvalidDocument" : errorKey.c_str());
        }
        s->inputActions.SetDocument(std::move(parsed));
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_input_action_button(
    FLARE_Session handle, const char* action, uint32_t length, int32_t* down, int32_t* pressed) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!action || length == 0 || length > 128u || !ValidUtf8(action, length) || (!down && !pressed))
        {
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        }
        const std::string_view name(action, length);
        if (down) *down = s->inputActions.IsActionDown(name) ? 1 : 0;
        if (pressed) *pressed = s->inputActions.IsActionPressed(name) ? 1 : 0;
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_input_action_axis1d(
    FLARE_Session handle, const char* action, uint32_t length, float* value) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!action || length == 0 || length > 128u || !ValidUtf8(action, length) || !value)
        {
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        }
        *value = s->inputActions.GetAxis1D(std::string_view(action, length));
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_input_action_axis2d(
    FLARE_Session handle, const char* action, uint32_t length, FLARE_Vector2* value) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!action || length == 0 || length > 128u || !ValidUtf8(action, length) || !value)
        {
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        }
        const flare::Vec2 v = s->inputActions.GetAxis2D(std::string_view(action, length));
        *value = {v.x, v.y};
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_input_get_mouse_delta(FLARE_Session handle, FLARE_Vector2* delta) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!delta) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *delta = s->mouseDelta;
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_input_set_cursor_mode(FLARE_Session handle, int32_t locked, int32_t visible) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        const bool nextLocked = locked != 0;
        const bool nextVisible = visible != 0;
        const bool wasLocked = s->cursorLocked;
        if (!nextLocked && wasLocked)
            ReleaseSessionCursor(*s);
        s->cursorLocked = nextLocked;
        s->cursorVisible = nextVisible;
        if (nextLocked && !wasLocked)
            s->cursorLockPausedByEsc = false;
        if (!nextLocked && nextVisible)
            ReleaseSessionCursor(*s);
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_input_get_cursor_mode(FLARE_Session handle, int32_t* locked, int32_t* visible) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (locked) *locked = (s->cursorLocked && !s->cursorLockPausedByEsc) ? 1 : 0;
        if (visible) *visible = s->cursorVisible ? 1 : 0;
        return FLARE_OK;
    });
}
