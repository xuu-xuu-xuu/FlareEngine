#pragma once
#include <stdint.h>

#if defined(_WIN32)
#define FLARE_CALL __cdecl
#if defined(FLARE_NATIVE_EXPORTS)
#define FLARE_API __declspec(dllexport)
#else
#define FLARE_API __declspec(dllimport)
#endif
#else
#define FLARE_CALL
#define FLARE_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
#define FLARE_NOEXCEPT noexcept
extern "C" {
#else
#define FLARE_NOEXCEPT
#endif

// ABI 只使用定宽数字、顺序布局和调用期间借用的缓冲区，绝不跨边界释放内存。
enum FLARE_Result {
    FLARE_OK = 0, FLARE_INVALID_ARGUMENT = 1, FLARE_INVALID_HANDLE = 2,
    FLARE_BUFFER_TOO_SMALL = 3, FLARE_BUSY = 4, FLARE_CALLBACK_FAILED = 5,
    FLARE_WRONG_THREAD = 6, FLARE_OUT_OF_MEMORY = 7, FLARE_INTERNAL_ERROR = 8
};
typedef uint64_t FLARE_Session;
FLARE_API int32_t FLARE_CALL flare_logic_operation(FLARE_Session session,uint32_t operation,uint64_t object,
    uint64_t target,double value,uint64_t* out_token,double* out_value) FLARE_NOEXCEPT;
typedef uint64_t FLARE_Entity;
typedef uint64_t FLARE_Body;
typedef struct FLARE_Vector3 { float x,y,z; } FLARE_Vector3;
typedef struct FLARE_Vector2 { float x,y; } FLARE_Vector2;
typedef struct FLARE_BoxBodyDesc {
    FLARE_Vector3 half_extents;
    float mass, friction, restitution;
    uint32_t layer, mask;
} FLARE_BoxBodyDesc;
typedef struct FLARE_SphereBodyDesc {
    float radius;
    float mass, friction, restitution;
    uint32_t layer, mask;
    int32_t is_trigger;
    uint32_t reserved;
} FLARE_SphereBodyDesc;
typedef struct FLARE_RayHit {
    FLARE_Body body;
    uint64_t entity_stable_id;
    FLARE_Vector3 point, normal;
    float distance;
    uint32_t reserved;
} FLARE_RayHit;
typedef struct FLARE_ContactEvent {
    uint64_t entity_a, entity_b;
    int32_t began, ended;
    FLARE_Vector3 point, normal;
    float impulse;
    uint32_t reserved;
} FLARE_ContactEvent;
typedef struct FLARE_TriggerEvent {
    uint64_t trigger_entity, other_entity;
    int32_t entered;
    uint32_t reserved;
} FLARE_TriggerEvent;
typedef uint64_t FLARE_AudioVoice;
typedef struct FLARE_AudioPlayDesc {
    FLARE_Vector3 position;
    float volume, pitch, min_distance_meters, max_distance_meters, spatial_blend;
    int32_t loop, priority;
} FLARE_AudioPlayDesc;
typedef struct FLARE_UiStyle {
    float anchor_min_x,anchor_min_y,anchor_max_x,anchor_max_y,pivot_x,pivot_y;
    float offset_x,offset_y,width,height,padding,spacing;
    int32_t layout,clip;
} FLARE_UiStyle;
typedef struct FLARE_InputState { uint8_t down[256],pressed[256],released[256]; int32_t keyboard_consumed; } FLARE_InputState;
typedef struct FLARE_PointerState { int32_t x,y; uint32_t down,pressed,released; float wheel; int32_t consumed; } FLARE_PointerState;
typedef struct FLARE_CharacterDesc { float radius,segment_half_height,walk_speed,jump_speed,maximum_push_impulse; } FLARE_CharacterDesc;
typedef struct FLARE_CharacterState { FLARE_Vector3 position; float vertical_velocity; int32_t grounded; } FLARE_CharacterState;
typedef struct FLARE_ModelInfo { uint32_t meshes,vertices,indices,reserved; } FLARE_ModelInfo;
typedef struct FLARE_Transform {
    float px, py, pz;
    float qx, qy, qz, qw;
    float sx, sy, sz;
} FLARE_Transform;

typedef struct FLARE_BodyState {
    FLARE_Transform transform;
    FLARE_Vector3 linear_velocity,angular_velocity;
    float mass;
    int32_t sleeping;
} FLARE_BodyState;
typedef struct FLARE_AbiInfo {
    uint32_t version;
    uint32_t transform_size;
    uint32_t pointer_size;
    uint32_t reserved;
} FLARE_AbiInfo;
typedef int32_t (FLARE_CALL *FLARE_UpdateCallback)(FLARE_Session session, double delta_seconds, void* context);
// component 为 0 时 phase=0x100|EnvironmentPhase；否则为组件句柄和 TickStage。
typedef int32_t (FLARE_CALL *FLARE_TickCallback)(FLARE_Session session,uint64_t component,uint32_t phase,double delta_seconds,uint64_t frame,uint64_t fixed_step,void* context);
typedef struct FLARE_TickPrerequisite {uint64_t task;uint32_t mode,reserved;} FLARE_TickPrerequisite;
typedef struct FLARE_EventHeader {
    uint64_t world,channel,type,source,source_stable_id,target,scene,frame,fixed_step,sequence;
    uint32_t version,phase;
} FLARE_EventHeader;
typedef struct FLARE_EventStats {
    uint64_t accepted,delivered,rejected,dropped_targets,no_receiver,callback_failures,invalid_payload,pending,queued,pending_bytes,peak_pending,peak_bytes;
    uint32_t closed,reserved;
} FLARE_EventStats;
// 0=已调用，2=接收者不参与，3=载荷无效；其他值为接收回调失败。头/字节仅在调用期间借用。
typedef int32_t (FLARE_CALL *FLARE_EventCallback)(FLARE_Session session,uint64_t subscription,const FLARE_EventHeader* header,const uint8_t* bytes,uint32_t length,void* context);
// 成员仅在调用期间借用；停用后、物理代理与 GO 回收之前通知宿主。
typedef int32_t (FLARE_CALL *FLARE_SceneCallback)(FLARE_Session session,uint64_t scene,const FLARE_Entity* members,uint32_t count,const uint64_t* invalidated_ids,uint32_t invalidated_count,void* context);
typedef int32_t (FLARE_CALL *FLARE_EditorCallback)(FLARE_Session session, int32_t phase, void* context); // 0=Play，1=停止。
typedef int32_t (FLARE_CALL *FLARE_EditorDataCallback)(FLARE_Session session,uint64_t entity_stable_id,const char* request,uint32_t request_length,char* output,uint32_t capacity,uint32_t* required,void* context);

FLARE_API int32_t FLARE_CALL flare_get_abi_info(FLARE_AbiInfo* info) FLARE_NOEXCEPT;
// required 包含结尾零；诊断为稳定文本键。查询错误不会覆盖上一次错误。
FLARE_API int32_t FLARE_CALL flare_get_last_error(char* buffer, uint32_t capacity, uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_create(FLARE_Session* session) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_close(FLARE_Session session) FLARE_NOEXCEPT;
// 除 close 外，session 只能由创建线程调用；回调同步发生在 step 的同一线程。
FLARE_API int32_t FLARE_CALL flare_session_set_update(FLARE_Session session, FLARE_UpdateCallback callback, void* context) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_set_post_step(FLARE_Session session, FLARE_UpdateCallback callback, void* context) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_step(FLARE_Session session, double delta_seconds) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_environment_set_callback(FLARE_Session session,FLARE_TickCallback callback,void* context) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_environment_configure(FLARE_Session session,double fixed_seconds,uint32_t maximum_steps,int32_t automatic,int32_t simulate_physics) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_environment_set_paused(FLARE_Session session,int32_t paused) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_environment_release(FLARE_Session session) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_environment_stats(FLARE_Session session,uint64_t* fixed_steps,double* simulation_seconds,double* dropped_seconds) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_tod_snapshot(FLARE_Session session,char* buffer,uint32_t capacity,uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_tod_set_time(FLARE_Session session,int64_t day,double time,int32_t replay_boundaries) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_tod_set_rate(FLARE_Session session,double rate) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_environment_fixed_step(FLARE_Session session,double seconds) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_component_tick_register(FLARE_Session session,uint64_t component,uint32_t stage,int32_t simulation_time,int32_t run_when_paused,uint64_t* task) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_component_tick_remove(FLARE_Session session,uint64_t task) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_component_tick_prerequisites(FLARE_Session session,uint64_t task,const FLARE_TickPrerequisite* prerequisites,uint32_t count) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_is_pending(FLARE_Session session,FLARE_Entity entity,int32_t* pending) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_script_component_pending(FLARE_Session session,uint64_t component,int32_t* pending) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_event_set_callback(FLARE_Session session,FLARE_EventCallback callback,void* context) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_event_register_type(FLARE_Session session,uint64_t type,uint32_t version,const char* key,uint32_t key_length,uint32_t maximum_bytes) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_event_publisher_create(FLARE_Session session,uint64_t* publisher) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_event_publisher_close(uint64_t publisher) FLARE_NOEXCEPT;
// 可在工作线程调用；publisher 是弱上下文句柄，不持有 session 或 World。
FLARE_API int32_t FLARE_CALL flare_event_publish(uint64_t publisher,uint64_t type,uint32_t version,uint32_t phase,const uint8_t* bytes,uint32_t length,FLARE_Entity source,uint64_t source_stable_id,FLARE_Entity target,uint64_t scene,uint32_t* post_result) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_event_subscribe(FLARE_Session session,uint64_t publisher,uint64_t component,uint64_t type,FLARE_Entity target,uint64_t scene,uint64_t* subscription) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_event_unsubscribe(FLARE_Session session,uint64_t subscription) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_event_stats(FLARE_Session session,FLARE_EventStats* stats) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_event_trace(FLARE_Session session,char* buffer,uint32_t capacity,uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_get_value(FLARE_Session session, double* value) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_set_value(FLARE_Session session, double value) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_get_step_count(FLARE_Session session, uint64_t* count) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_get_input(FLARE_Session session, FLARE_InputState* input) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_get_pointer(FLARE_Session session, FLARE_PointerState* input) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_input_load_actions(FLARE_Session session, const char* json_utf8, uint32_t length) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_input_action_button(FLARE_Session session, const char* action, uint32_t length, int32_t* down, int32_t* pressed) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_input_action_axis1d(FLARE_Session session, const char* action, uint32_t length, float* value) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_input_action_axis2d(FLARE_Session session, const char* action, uint32_t length, FLARE_Vector2* value) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_input_get_mouse_delta(FLARE_Session session, FLARE_Vector2* delta) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_input_set_cursor_mode(FLARE_Session session, int32_t locked, int32_t visible) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_input_get_cursor_mode(FLARE_Session session, int32_t* locked, int32_t* visible) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_run_window(FLARE_Session session, const char* locale, uint32_t length, int32_t visible, uint64_t maximum_frames) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_configure_quality(FLARE_Session session,const char* path,uint32_t length,int32_t show_menu) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_set_quality_profiles(FLARE_Session session,const char* json,uint32_t length) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_player_quality(FLARE_Session session,int32_t requested,int32_t* current) FLARE_NOEXCEPT;
// 空路径/零帧清除设置；非空路径必须指定大于零的宿主更新帧号，只能在窗口外配置。
FLARE_API int32_t FLARE_CALL flare_session_set_window_capture(FLARE_Session session,const char* path,uint32_t length,uint64_t frame) FLARE_NOEXCEPT;
// 普通 Player 的固定回放尺寸；0x0 清除。只在窗口外配置，交换链与客户区同步。
FLARE_API int32_t FLARE_CALL flare_session_set_window_resolution(FLARE_Session session,uint32_t width,uint32_t height) FLARE_NOEXCEPT;
// 固定步长（秒）；0 清除。序列抓帧时缺省为 1/30，保证每个宿主更新恰好推进整数个固定步。
FLARE_API int32_t FLARE_CALL flare_session_set_window_fixed_step(FLARE_Session session,double seconds) FLARE_NOEXCEPT;
// 连续固定步抓帧：目录 + 帧数，0 清除。只在窗口外配置，与单帧抓帧互斥。
FLARE_API int32_t FLARE_CALL flare_session_set_capture_sequence(FLARE_Session session,const char* path,uint32_t length,uint64_t count) FLARE_NOEXCEPT;
// 可选 Player 诊断；空路径/零尺寸关闭，活动窗口禁止修改，输出目录不覆盖。
FLARE_API int32_t FLARE_CALL flare_session_set_window_profile(FLARE_Session session,const char* path,uint32_t length,uint32_t width,uint32_t height) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_request_stop(FLARE_Session session) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_set_locale(FLARE_Session session, const char* locale, uint32_t length) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_text(FLARE_Session session, const char* key, uint32_t length, char* buffer, uint32_t capacity, uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_set_asset_root(FLARE_Session session,const char* root,uint32_t root_length,const char* cache,uint32_t cache_length) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_assets_list(FLARE_Session session,char* output,uint32_t capacity,uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_assets_load_model_info(FLARE_Session session,const char* id,uint32_t length,FLARE_ModelInfo* info) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_set_editor_callback(FLARE_Session session, FLARE_EditorCallback callback, void* context) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_run_editor(FLARE_Session session, const char* root, uint32_t root_length, const char* locale, uint32_t locale_length, int32_t visible, uint64_t maximum_frames, int32_t start_playing) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_editor_set_playing(FLARE_Session session, int32_t playing) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_session_set_editor_data(FLARE_Session session,FLARE_EditorDataCallback callback,void* context) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_editor_inspector_query(FLARE_Session session,uint64_t entity_stable_id,char* output,uint32_t capacity,uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_editor_inspector_apply(FLARE_Session session,uint64_t entity_stable_id,const char* request,uint32_t length) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_create(FLARE_Session session, const char* name_utf8, uint32_t name_length, FLARE_Entity* entity) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_destroy(FLARE_Session session, FLARE_Entity entity) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_is_alive(FLARE_Session session, FLARE_Entity entity, int32_t* alive) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_get_enabled(FLARE_Session session,FLARE_Entity entity,int32_t* enabled) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_set_enabled(FLARE_Session session,FLARE_Entity entity,int32_t enabled) FLARE_NOEXCEPT;
// 受控实例只传标识，不传托管对象或 C++ 继承布局；句柄仍为独立 64 位代数身份。
FLARE_API int32_t FLARE_CALL flare_script_component_attach(FLARE_Session session,FLARE_Entity owner,uint64_t controlled_id,int32_t enabled,uint64_t* component) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_script_component_remove(FLARE_Session session,uint64_t component) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_script_component_set_enabled(FLARE_Session session,uint64_t component,int32_t enabled) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_script_component_state(FLARE_Session session,uint64_t component,int32_t* alive,int32_t* active) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_script_component_count(FLARE_Session session,uint32_t* count) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_get_transform(FLARE_Session session, FLARE_Entity entity, FLARE_Transform* transform) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_set_transform(FLARE_Session session, FLARE_Entity entity, const FLARE_Transform* transform) FLARE_NOEXCEPT;
// W03：按稳定 ID 播放角色 clip；写回 actor 组件的可序列化属性，下一固定步生效。
FLARE_API int32_t FLARE_CALL flare_entity_actor_play(FLARE_Session session,uint64_t entity_stable_id,const char* clip,uint32_t length,float speed,int32_t loop) FLARE_NOEXCEPT;
// 世界空间关节位置；关节不存在时 found=0 且 position 不变（不是错误）。
FLARE_API int32_t FLARE_CALL flare_entity_joint_position(FLARE_Session session,uint64_t entity_stable_id,const char* joint,uint32_t length,FLARE_Vector3* position,int32_t* found) FLARE_NOEXCEPT;
// 给实体挂接或替换静态网格（项目在运行时布景/生长时使用）。
FLARE_API int32_t FLARE_CALL flare_entity_set_mesh(FLARE_Session session,FLARE_Entity entity,const char* asset_id,uint32_t length,int32_t casts_shadow,int32_t receives_shadow) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_set_camera_fov(FLARE_Session session,uint64_t entity_stable_id,float fov) FLARE_NOEXCEPT;
// W06：运行时外观覆盖；mode<0 清除覆盖并回到场景 RenderSettings 的值。
FLARE_API int32_t FLARE_CALL flare_session_set_look(FLARE_Session session,float mode,float blend) FLARE_NOEXCEPT;
// W07：世界方向投影到游戏相机的归一化屏幕坐标（左上原点，0..1）；不在前方时 in_front=0。
FLARE_API int32_t FLARE_CALL flare_camera_project_direction(FLARE_Session session,uint64_t camera_stable_id,FLARE_Vector3 world_direction,FLARE_Vector2* screen_uv,int32_t* in_front) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_camera_screen_point_to_ray(FLARE_Session session,uint64_t camera_stable_id,FLARE_Vector2 screen_pixel,FLARE_Vector3* origin,FLARE_Vector3* direction) FLARE_NOEXCEPT;
// W07：程序化天空覆盖（JSON）；空长度清除并回到引擎默认天空。
FLARE_API int32_t FLARE_CALL flare_session_set_sky(FLARE_Session session,const char* json,uint32_t length) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_get_stable_id(FLARE_Session session, FLARE_Entity entity, uint64_t* stable_id) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_find(FLARE_Session session, uint64_t stable_id, FLARE_Entity* entity) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_find_by_name(FLARE_Session session, const char* name_utf8, uint32_t length, FLARE_Entity* entity, int32_t* found) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_find_by_tag(FLARE_Session session, const char* tag_utf8, uint32_t length, FLARE_Entity* entity, int32_t* found) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_get_name(FLARE_Session session, FLARE_Entity entity, char* buffer, uint32_t capacity, uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_get_tag(FLARE_Session session, FLARE_Entity entity, char* buffer, uint32_t capacity, uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_set_tag(FLARE_Session session, FLARE_Entity entity, const char* tag_utf8, uint32_t length) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_get_layer(FLARE_Session session, FLARE_Entity entity, uint32_t* layer) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_set_layer(FLARE_Session session, FLARE_Entity entity, uint32_t layer) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_get_world_transform(FLARE_Session session, FLARE_Entity entity, FLARE_Transform* transform) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_get_parent(FLARE_Session session, FLARE_Entity entity, FLARE_Entity* parent) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_set_parent(FLARE_Session session, FLARE_Entity entity, FLARE_Entity parent, int32_t keep_world) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_entity_duplicate(FLARE_Session session, FLARE_Entity source, const FLARE_Vector3* override_pos, const float* override_rot_xyzw, uint64_t* out_source_stable_ids, FLARE_Entity* out_cloned_entities, uint32_t capacity, uint32_t* out_count) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_export(FLARE_Session session, char* buffer, uint32_t capacity, uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_import(FLARE_Session session, const char* json_utf8, uint32_t length) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_set_callback(FLARE_Session session,FLARE_SceneCallback callback,void* context) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_create(FLARE_Session session,const char* name,uint32_t length,uint64_t* scene) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_request_file(FLARE_Session session,uint64_t scene,const char* path,uint32_t length,uint32_t mode,uint64_t replacing,uint64_t* request) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_request_json(FLARE_Session session,uint64_t scene,const char* json,uint32_t length,uint32_t mode,uint64_t replacing,uint64_t* request) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_cancel(FLARE_Session session,uint64_t scene) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_unload(FLARE_Session session,uint64_t scene) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_get(FLARE_Session session,uint64_t scene,char* buffer,uint32_t capacity,uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_stats(FLARE_Session session,char* buffer,uint32_t capacity,uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_active(FLARE_Session session,uint64_t* scene) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_of_entity(FLARE_Session session,FLARE_Entity entity,uint64_t* scene) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_create_entity(FLARE_Session session,uint64_t scene,const char* name,uint32_t length,FLARE_Entity* entity) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_scene_callback_scope(FLARE_Session session,uint64_t scene,uint64_t* previous) FLARE_NOEXCEPT;
// 每实体最多一个 box body，只允许根实体；物理拥有位置/旋转写权，移动必须使用 teleport。
FLARE_API int32_t FLARE_CALL flare_body_create(FLARE_Session session, FLARE_Entity entity, const FLARE_BoxBodyDesc* description, FLARE_Body* body) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_body_create_sphere(FLARE_Session session, FLARE_Entity entity, const FLARE_SphereBodyDesc* description, FLARE_Body* body) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_body_set_trigger(FLARE_Session session, FLARE_Body body, int32_t is_trigger) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_body_is_trigger(FLARE_Session session, FLARE_Body body, int32_t* is_trigger) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_body_find(FLARE_Session session, FLARE_Entity entity, FLARE_Body* body) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_character_component_state(FLARE_Session session, FLARE_Entity entity, FLARE_CharacterState* state) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_body_destroy(FLARE_Session session, FLARE_Body body) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_body_get_state(FLARE_Session session, FLARE_Body body, FLARE_BodyState* state) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_body_set_velocity(FLARE_Session session, FLARE_Body body, FLARE_Vector3 linear, FLARE_Vector3 angular) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_body_apply_force(FLARE_Session session, FLARE_Body body, FLARE_Vector3 force, FLARE_Vector3 world_point, int32_t impulse) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_body_teleport(FLARE_Session session, FLARE_Body body, const FLARE_Transform* transform, int32_t clear_velocity) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_physics_step(FLARE_Session session, double delta_seconds) FLARE_NOEXCEPT;
// 原子替换调试绘制状态并返回旧值，物理模拟不受影响。
FLARE_API int32_t FLARE_CALL flare_physics_set_debug_draw(FLARE_Session session, int32_t enabled, int32_t* previous) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_physics_advance(FLARE_Session session, double delta_seconds, uint32_t* steps, double* dropped_seconds) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_physics_raycast(FLARE_Session session, FLARE_Vector3 origin, FLARE_Vector3 direction, float distance, uint32_t mask, FLARE_RayHit* hit, int32_t* found) FLARE_NOEXCEPT;
// 查询数量不会消费事件；缓冲区足够时一次复制并清空，回调由托管层在步骤结束后分发。
FLARE_API int32_t FLARE_CALL flare_physics_drain_contacts(FLARE_Session session, FLARE_ContactEvent* buffer, uint32_t capacity, uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_physics_drain_triggers(FLARE_Session session, FLARE_TriggerEvent* buffer, uint32_t capacity, uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_character_create(FLARE_Session session, FLARE_Entity entity, const FLARE_CharacterDesc* description) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_character_move(FLARE_Session session, FLARE_Entity entity, FLARE_Vector3 direction, int32_t jump, float seconds, FLARE_CharacterState* state) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_character_teleport(FLARE_Session session, FLARE_Entity entity, FLARE_Vector3 position) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_character_destroy(FLARE_Session session, FLARE_Entity entity) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_create(FLARE_Session session, int32_t kind, uint64_t parent, uint64_t* id) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_remove(FLARE_Session session, uint64_t id) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_set_style(FLARE_Session session, uint64_t id, const FLARE_UiStyle* style) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_set_text(FLARE_Session session, uint64_t id, const char* text, uint32_t length) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_set_image(FLARE_Session session, uint64_t id, const char* asset_id, uint32_t length) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_exists(FLARE_Session session, uint64_t id, int32_t* exists) FLARE_NOEXCEPT;
// entity=0 且两个 ID 长度为 0 时清除该事件；其他情况遵守 UiEventBinding schema。
FLARE_API int32_t FLARE_CALL flare_ui_bind_action(FLARE_Session session,uint64_t id,int32_t event_kind,uint64_t entity,
    const char* script,uint32_t script_length,const char* action,uint32_t action_length) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_read_bindings(FLARE_Session session,uint64_t id,char* buffer,uint32_t capacity,uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_fade(FLARE_Session session, uint64_t id, float opacity, double seconds) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_move(FLARE_Session session, uint64_t id, float x, float y, double seconds) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_color(FLARE_Session session, uint64_t id, float r, float g, float b, float a, double seconds) FLARE_NOEXCEPT;
// property: 0=value, 1=visible, 2=enabled, 3=focus, 4=blocksPointer（仅 0/1）, 5=step, 6=cancelTransitions（仅 1）。range 使用独立接口。
FLARE_API int32_t FLARE_CALL flare_ui_set_property(FLARE_Session session, uint64_t id, int32_t property, double value) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_set_range(FLARE_Session session, uint64_t id, double minimum, double maximum) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_layout(FLARE_Session session, float width, float height) FLARE_NOEXCEPT;
// operation: 0=move,1=down,2=up,3=wheel,4=cancel；consumed 用于输入路由。
FLARE_API int32_t FLARE_CALL flare_ui_pointer(FLARE_Session session, int32_t operation, float x, float y, float wheel, int32_t* consumed) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_key(FLARE_Session session, uint32_t key, int32_t shift, int32_t control, int32_t* consumed) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_input_text(FLARE_Session session, const char* text, uint32_t length, int32_t composition, int32_t* consumed) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_consumes_keyboard(FLARE_Session session, int32_t* consumed) FLARE_NOEXCEPT;
// JSON 输出 caller-owned；kind: 0=文档，1=视觉快照，2=待派发事件（成功复制后消费）。
FLARE_API int32_t FLARE_CALL flare_ui_read_json(FLARE_Session session, int32_t kind, char* buffer, uint32_t capacity, uint32_t* required) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_ui_load_json(FLARE_Session session, const char* text, uint32_t length) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_play(FLARE_Session session, const char* asset_id, uint32_t length, const FLARE_AudioPlayDesc* description, FLARE_AudioVoice* voice) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_stop_voice(FLARE_Session session, FLARE_AudioVoice voice) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_is_voice_playing(FLARE_Session session, FLARE_AudioVoice voice, int32_t* playing) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_set_voice_parameters(FLARE_Session session, FLARE_AudioVoice voice, const FLARE_AudioPlayDesc* description) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_play_source(FLARE_Session session, FLARE_Entity entity) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_stop_source(FLARE_Session session, FLARE_Entity entity) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_is_source_playing(FLARE_Session session, FLARE_Entity entity, int32_t* playing) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_set_master_volume(FLARE_Session session, float volume) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_get_master_volume(FLARE_Session session, float* volume) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_set_paused(FLARE_Session session, int32_t paused) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_stop_all(FLARE_Session session) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_active_voice_count(FLARE_Session session, uint32_t* count) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_audio_use_null_backend(FLARE_Session session, int32_t force_null) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_particles_emit_burst(FLARE_Session session, FLARE_Entity entity, uint32_t count, uint32_t* spawned) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_particles_set_playing(FLARE_Session session, FLARE_Entity entity, int32_t playing) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_particles_clear(FLARE_Session session, FLARE_Entity entity) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_particles_active_count(FLARE_Session session, FLARE_Entity entity, uint32_t* count) FLARE_NOEXCEPT;
typedef struct FLARE_NavPathInfo {
    int32_t status;
    uint32_t waypoint_count;
    float total_length_meters;
} FLARE_NavPathInfo;
FLARE_API int32_t FLARE_CALL flare_particles_emitter_count(FLARE_Session session, uint32_t* count) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_nav_find_path(
    FLARE_Session session,
    const char* nav_asset_id,
    uint32_t nav_asset_length,
    FLARE_Vector3 start,
    FLARE_Vector3 goal,
    FLARE_Vector3* out_waypoints,
    uint32_t max_waypoints,
    FLARE_NavPathInfo* out_info) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_nav_set_destination(FLARE_Session session, FLARE_Entity entity, FLARE_Vector3 target) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_nav_stop_agent(FLARE_Session session, FLARE_Entity entity) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_nav_remaining_distance(FLARE_Session session, FLARE_Entity entity, float* out_meters) FLARE_NOEXCEPT;
FLARE_API int32_t FLARE_CALL flare_nav_agent_status(FLARE_Session session, FLARE_Entity entity, int32_t* out_status) FLARE_NOEXCEPT;

#ifdef __cplusplus
}
#endif
