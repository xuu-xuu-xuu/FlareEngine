# 自定义 Shader 与 ASE 可视化连线（热编译与实时预览）

在 Flare 引擎里，写自定义着色器就像写 C# 脚本一样顺手——你不需要碰繁琐的 D3D12 根签名、常量缓冲对齐、级联阴影采样或蒙皮矩阵，提供**两种完全互通的创作方式**：

1. **代码派（`.fshader.hlsl` 表面着色器）**：采用游戏开发者一眼就能看懂的 **Unity Surface Shader 简明结构**（`Shader` / `Properties` / `SubShader` / `void surf(Input IN, inout SurfaceOutput o)`），右键新建即可直接上手，在外部编辑器按 `Ctrl + S` 保存后**切回 Flare 窗口即自动热编译生效**。
2. **连线派（`.fshader.json` ASE 可视化着色器）**：像 Unity Amplify Shader Editor（ASE）一样拖拽节点连线（内置 **9 大分类 138 个节点**），**左侧场景视口 (`Viewport`) 里的真实模型实时同步刷新**，连线面板右下角还自带一个**可拖拽旋转光源的迷你动态预览球**。

无论是手写 `.fshader.hlsl` 还是连线 `.fshader.json`，都自动接入引擎完整渲染管线（方向光级联软阴影、多光源分簇照明、室内局部阴影、GTAO、SSGI、IBL 环境反射、透明混合、骨骼动画蒙皮），并且打包导出的独立游戏 `Play.exe` 原生支持，零额外配置。

---

## 一、像写 C# 一样写 `.fshader.hlsl`（3 步跑通）

### 第 1 步：右键新建自定义 Shader 与配套材质

1. 在下方 **资源 (`Assets`)** 面板点击工具栏 **`＋`**（或在空白处右键），选择 **新建表面着色器 (HLSL) (`New Surface Shader (HLSL)`)**，生成 `NewSurfaceShader.fshader.hlsl`。
2. 选中刚生成的着色器，在右侧 **属性 (`Inspector`)** 面板点击 **以此着色器新建材质资产 (`Create Material Using This Shader`)**：
   - 编辑器会自动在同目录生成 `NewSurfaceShader_Mat.material.json` 并填好着色器引用与默认参数；
   - 如果你在场景里正选中某个网格物体，还会**自动把新材质赋给该物体**！
   - 你也可以随时把任意 `.fshader.hlsl` 或 `.fshader.json` 从 **资源 (`Assets`)** 拖进任意材质的 **Shader 产物 (`Shader artifact`)** 槽位（或点槽位右侧 `⊙` 选择器筛选切换）。

### 第 2 步：外部编辑器保存，切回窗口零重启热编译

双击 `.fshader.hlsl`（或点属性面板的 **在代码编辑器中打开 HLSL (`Open HLSL in Code Editor`)**）即可在你常用的 VS Code / Cursor / Visual Studio 中打开：

```text
  在外部编辑器修改 .fshader.hlsl 按 Ctrl+S 保存
                       │
                       ▼
           直接切回 Flare 编辑器窗口
                       │
                       ▼
  后台自动热编译 ──▶ 成功：视口与运行预览模型瞬间刷新，材质面板自动同步新滑块
                ──▶ 报错：视口保留上一版正常画面绝不黑屏，双击控制台报错行直达代码行列
```

::: tip 写错语法也绝不黑屏，双击控制台直达出错行
当 HLSL 存在拼写或类型错误时，渲染器会自动保留上一次编译成功的管线状态（PSO），你的 3D 场景不会闪烁或变黑；同时左下角 **控制台 (`Console`)** 会输出精准到源文件的 `文件名.fshader.hlsl(行,列): error ...`，**双击该报错行**即可直接把外部编辑器光标跳到出错行。
:::

### 第 3 步：在材质面板拖滑块与拾色器实时调参

只要在 `Properties { ... }` 块里声明属性（写法与 Unity 完全一致），选中引用该着色器的 `.material.json` 时，右侧 **属性 (`Inspector`)** 面板就会自动生成 **自定义着色器参数 (`Custom Shader Parameters`)** 分组——支持范围滑块、布尔开关、浮点拖拽、颜色拾色器与向量输入，支持 `Ctrl + Z / Y` 撤销重做，拖动时左侧视口模型零延迟实时响应。

---

## 二、`.fshader.hlsl` 极简语法手册（Unity Surface 风格）

右键 **新建表面着色器 (HLSL) (`New Surface Shader (HLSL)`)** 时，引擎会生成一份像 Unity `Standard Surface Shader` 一样干净直观的初始模板——没有冗长的底层宏与寄存器绑定，也无需在 `HLSLPROGRAM` 内重复声明 `Properties` 变量：

```hlsl
Shader "NewSurfaceShader"
{
    Properties
    {
        _Color      ("基础颜色", Color)       = (1, 1, 1, 1)
        _Metallic   ("金属度",   Range(0, 1)) = 0.0
        _Smoothness ("光滑度",   Range(0, 1)) = 0.5
    }

    SubShader
    {
        // 光照: Lit (写实PBR) | Toon (卡通) | Unlit (无光照)
        // 混合: Opaque (不透明) | Cutout (镂空) | Transparent (半透明)
        // 剔除: Back (单面) | Off (双面)
        Shading Lit
        Blend   Opaque
        Cull    Back

        HLSLPROGRAM

        void surf(Input IN, inout SurfaceOutput o)
        {
            float4 c = tex2D(_MainTex, IN.uv) * _Color;
            o.Albedo     = c.rgb;
            o.Metallic   = _Metallic;
            o.Smoothness = _Smoothness;
            o.Alpha      = c.a;
        }

        ENDHLSL
    }
}
```

### 1. `Properties` 属性声明一览

`Properties` 中声明的变量**自动注入 `HLSLPROGRAM`**，你在 `surf` 或 `vert` 里直接使用同名变量即可（单材质最多支持打包 **8 个 `float4` 槽位 / 32 个标量参数**）：

| 属性写法示例 | HLSL 类型 | 材质面板控件 |
| :--- | :--- | :--- |
| `_Color ("基础颜色", Color) = (1, 1, 1, 1)` | `float4` | HSV 可视化拾色器 |
| `_Smoothness ("光滑度", Range(0, 1)) = 0.5` | `float` | 带上下限夹紧的滑动条 |
| `_Speed ("速度", Float) = 2.0` | `float` | 可左右拖拽微调的浮点框 |
| `[Toggle] _UseRim ("边缘光开关", Float) = 1` | `float` | `0 ~ 1` 开关滑块 |
| `_Tiling ("UV 平铺", Vector2) = (2, 2)` | `float2` | 二维向量输入 |
| `_Direction ("风向", Vector3) = (1, 0, 0)` | `float3` | 三维向量输入 |
| `_Extra ("四维参数", Vector) = (1, 1, 0, 0)` | `float4` | 四维向量输入 |

> **兼容提示**：从 Unity 粘贴代码时常见的 `_MainTex ("Albedo", 2D) = "white" {}`、`CGPROGRAM ... ENDCG`、`#pragma surface ...`、`fixed4`、`struct Input { ... };` 均已原生兼容，无需手动删改即可直接编译运行。

### 2. `SubShader` / `Pass` 渲染状态

| 状态指令 | 可选值 | 说明 |
| :--- | :--- | :--- |
| `Shading` | `Lit`（默认）/ `Toon` / `Unlit` | `Lit` 走完整物理 PBR 光照与软阴影；`Toon` 为柔和卡通色阶光照；`Unlit` 仅输出无光照颜色与自发光。 |
| `Blend` | `Opaque` / `Cutout` / `Transparent` / `Additive` | 不透明、Alpha 镂空、标准半透明混合（兼容 `SrcAlpha OneMinusSrcAlpha`）或发光叠加（兼容 `SrcAlpha One` / `One One`）。 |
| `Cull` | `Back`（默认）/ `Front` / `Off` | `Back` 背面剔除；**`Front` 正面剔除**（背面法线外扩卡通描边专用）；`Off` 双面渲染。 |
| `ZWrite` | `On`（默认）/ `Off` | 是否写入深度缓冲（叠加流光壳、半透明护盾常用 `ZWrite Off`）。 |
| `ZTest` | `LEqual`（默认）/ `Greater` / `Always` | 深度测试比较方式（**`ZTest Greater` 专用于角色被墙体遮挡时的 X-Ray 隔墙透视轮廓**）。 |

### 3. 内置贴图采样与动画时间

- **贴图采样（自动读取材质面板绑定的贴图槽位）**：
  - `tex2D(_MainTex, IN.uv)`（或 `SampleMainTex(IN.uv)`）：采样材质基础色贴图，返回 `float4`
  - `UnpackNormal(tex2D(_BumpMap, IN.uv))`（或 `SampleNormalTS(IN.uv)`）：采样并解包切线空间法线，返回 `float3`
  - `tex2D(_MetallicGlossMap, IN.uv)`：采样金属粗糙度贴图（`.r` 金属度，`.g` 粗糙度，`.a` 光滑度）
- **Unity 风格全局动画时间（编辑视口与运行模式均实时递增）**：
  - `_Time`：`float4(t/20, t, t*2, t*3)`，常用 **`_Time.y` 表示当前累计秒数 `t`**
  - `_SinTime`：`float4(sin(t/8), sin(t/4), sin(t/2), sin(t))`
  - `_CosTime`：`float4(cos(t/8), cos(t/4), cos(t/2), cos(t))`
  - `unity_DeltaTime.x`：当前帧间隔 `dt`

### 4. `surf(Input IN, inout SurfaceOutput o)` 字段速查

| 字段 | 类型 | 含义 |
| :--- | :--- | :--- |
| `IN.uv`（别名 `IN.uv_MainTex`） | `float2` | 模型主 UV 坐标 |
| `IN.worldPos` / `IN.localPos` / `IN.objectPos` | `float3` | 像素世界坐标 / 模型局部坐标 / 物体枢轴世界原点 |
| `IN.worldNormal` | `float3` | 世界空间法线 |
| `IN.viewDir` / `IN.lightDir` | `float3` | 指向相机的单位视线向量 / 指向主光源的单位向量 |
| `IN.lightColor` | `float3` | 主光源辐射颜色 |
| `IN.screenUV` | `float2` | 屏幕归一化坐标 `[0, 1]` |
| `IN.color` | `float4` | 顶点颜色 |
| `IN.facing` | `float` | 正背面符号（正面 `+1.0`，背面 `-1.0`） |
| `o.Albedo` | `float3` | 固有反照率颜色（默认材质基础色） |
| `o.Normal` | `float3` | 切线空间法线（默认 `float3(0, 0, 1)`） |
| `o.Emission` | `float3` | 自发光颜色（支持大于 `1.0` 触发 HDR Bloom 辉光） |
| `o.Metallic` | `float` | 金属度 `[0, 1]` |
| `o.Smoothness`（或 `o.Roughness`） | `float` | 光滑度 `[0, 1]`（`1` 为镜面光滑；习惯写 `o.Roughness` 同样支持） |
| `o.Alpha` | `float` | 不透明度 `[0, 1]`（配合 `Cutout` 或 `Transparent`） |
| `o.AlphaCutoff` | `float` | `Cutout` 镂空阈值（默认取材质 Cutoff，`o.Alpha < o.AlphaCutoff` 的像素与阴影同步镂空） |

### 5. 实战示例 1：半透明全息能量护盾（菲涅尔边缘发光 + 扫描线）

```hlsl
Shader "FX/HologramShield"
{
    Properties
    {
        _RimColor  ("边缘发光色", Color)          = (0.15, 0.80, 1.00, 1.0)
        _RimPower  ("菲涅尔锐度", Range(0.5, 8.0)) = 3.0
        _ScanSpeed ("扫描速度",   Range(0.0, 10.0)) = 2.5
        _Opacity   ("整体透明度", Range(0.05, 1.0)) = 0.45
    }

    SubShader
    {
        Shading Lit
        Blend   Transparent
        Cull    Off

        HLSLPROGRAM

        void surf(Input IN, inout SurfaceOutput o)
        {
            float ndotv = saturate(dot(normalize(IN.worldNormal), normalize(IN.viewDir)));
            float rim   = pow(1.0 - ndotv, _RimPower);
            float scan  = 0.65 + 0.35 * sin(IN.worldPos.y * 18.0 - _Time.y * _ScanSpeed);

            o.Albedo     = _RimColor.rgb * 0.25;
            o.Emission   = _RimColor.rgb * (rim * 2.2 + 0.25) * scan;
            o.Smoothness = 0.85;
            o.Alpha      = saturate(_Opacity * (0.35 + rim * 0.85));
        }

        ENDHLSL
    }
}
```

### 6. 实战示例 2：带阴影同步的顶点波浪 `vert(inout VertexInput v)`

需要做水面波浪、旗帜飘动、受击膨胀时，只需在 `HLSLPROGRAM` 中多写一个 `void vert(inout VertexInput v)` 修改 `v.vertex`（可读 `v.vertex`、`v.normal`、`v.uv` 与 `_Time`）——**主渲染与方向光阴影 Pass 都会自动执行它**，物体投在地上的影子也会跟着一起波动：

```hlsl
Shader "Stylized/WaveWater"
{
    Properties
    {
        _Shallow  ("浅水色",   Color)           = (0.12, 0.72, 0.88, 1.0)
        _Deep     ("深水色",   Color)           = (0.03, 0.22, 0.48, 1.0)
        _WaveAmp  ("波浪高度", Range(0.0, 0.5)) = 0.12
        _WaveFreq ("波浪频率", Range(0.5, 10.0)) = 3.5
    }

    SubShader
    {
        Shading Toon
        Blend   Opaque
        Cull    Back

        HLSLPROGRAM

        void vert(inout VertexInput v)
        {
            float wave = sin(v.vertex.x * _WaveFreq + _Time.y * 2.4)
                       * cos(v.vertex.z * _WaveFreq + _Time.y * 1.9);
            v.vertex.y += wave * _WaveAmp;
        }

        void surf(Input IN, inout SurfaceOutput o)
        {
            float fresnel = pow(1.0 - saturate(dot(normalize(IN.worldNormal), normalize(IN.viewDir))), 3.0);
            o.Albedo     = lerp(_Deep.rgb, _Shallow.rgb, saturate(0.35 + fresnel));
            o.Smoothness = 0.88;
        }

        ENDHLSL
    }
}
```

### 7. 多 Pass 着色器（卡通描边 / 双层流光外壳 / 隔墙 X-Ray 透视）

在 `SubShader { ... }` 内可以编写多个独立的 `Pass { ... }` 块（单个着色器最多支持 **8 个 Pass**），并可用 `HLSLINCLUDE ... ENDHLSL` 编写所有 Pass 共享的工具函数；引擎还会智能挑选非 `Cull Front` 的主体不透明 Pass 投射方向光阴影，避免外扩描边把角色影子撑大。

下面是一份可直接复制使用的 **3-Pass 卡通角色着色器（卡通受光主体 + 背面法线外扩描边 + 隔墙遮挡 X-Ray 透视光影）**：

```hlsl
Shader "Character/ToonOutlineXRay"
{
    Properties
    {
        _Color        ("主体颜色",   Color)           = (1.00, 0.86, 0.82, 1.0)
        _OutlineColor ("描边颜色",   Color)           = (0.08, 0.05, 0.12, 1.0)
        _OutlineWidth ("描边宽度",   Range(0.0, 0.1)) = 0.025
        _XRayColor    ("隔墙透视色", Color)           = (0.10, 0.85, 1.00, 1.0)
    }

    SubShader
    {
        HLSLINCLUDE
        float CalcRim(float3 normalWS, float3 viewWS, float power)
        {
            return pow(1.0 - saturate(dot(normalize(normalWS), normalize(viewWS))), power);
        }
        ENDHLSL

        // Pass 1：卡通受光主体（自动承担方向光阴影投射）
        Pass
        {
            Name "ToonBody"
            Shading Toon
            Cull    Back

            HLSLPROGRAM
            void surf(Input IN, inout SurfaceOutput o)
            {
                float4 c = tex2D(_MainTex, IN.uv) * _Color;
                o.Albedo     = c.rgb;
                o.Smoothness = 0.35;
            }
            ENDHLSL
        }

        // Pass 2：背面法线外扩描边（Cull Front 仅画外扩后的背面轮廓）
        Pass
        {
            Name "Outline"
            Shading Unlit
            Cull    Front

            HLSLPROGRAM
            void vert(inout VertexInput v)
            {
                v.vertex += v.normal * _OutlineWidth;
            }

            void surf(Input IN, inout SurfaceOutput o)
            {
                o.Albedo = _OutlineColor.rgb;
            }
            ENDHLSL
        }

        // Pass 3：隔墙 X-Ray 透视轮廓（仅在被前方墙体遮挡 ZTest Greater 时发光叠加）
        Pass
        {
            Name "BehindWallXRay"
            Shading Unlit
            Blend   Additive
            ZWrite  Off
            ZTest   Greater
            Cull    Back

            HLSLPROGRAM
            void surf(Input IN, inout SurfaceOutput o)
            {
                float rim  = CalcRim(IN.worldNormal, IN.viewDir, 2.2);
                o.Albedo   = 0.0;
                o.Emission = _XRayColor.rgb * (0.25 + rim * 1.4);
                o.Alpha    = 1.0;
            }
            ENDHLSL
        }
    }
}
```

---

## 三、ASE 风格可视化连线（`Shader Graph`）

不想记 HLSL 函数名？直接用内置的 **着色器连线 (`Shader Graph`)** 连线即可！

### 1. 新建并打开连线面板（双视口实时联动）

1. 在 **资源 (`Assets`)** 面板点击 **`＋`**（或右键）→ **新建可视化着色器 (ASE) (`New Visual Shader Graph (ASE)`)**，生成 `NewShaderGraph.fshader.json`。
2. 创建或双击打开 `.fshader.json` 时，面板会自动停靠在左下方页签组（与 **摄像机视口**、**控制台** 并列），**左上方场景视口 (`Viewport`) 完全不被遮挡**；你也可以随时通过顶栏 **面板 (`Panels`) → 着色器连线 (`Shader Graph`)** 打开，或把标签拖出来变成独立副屏窗口。
3. 点击连线面板顶栏右侧的 **新建配套材质 (`Create Material`)**（若正选中场景物体，会自动把新材质赋给它）。
4. 此后你在画布里的**每一次连线、断开、暴露参数、修改常量或撤销重做**：
   - **左上方场景视口 (`Viewport`)** 里使用该材质的真实场景模型**立刻热编译同步变化**；
   - **连线面板右下角**的 `112×112` **迷你材质预览球**同步动画刷新，按住预览球左右/上下拖动即可实时旋转预览光源角度！

### 2. 画布手势与高效连线操作

| 操作 | 手势 / 快捷键 |
| :--- | :--- |
| **平移画布** | 按住 **鼠标中键**、**鼠标右键拖拽** 或 `Alt + 左键拖拽` |
| **以光标为中心缩放** | 滚动 **鼠标滚轮**（`0.35× ~ 2.0×`） |
| **呼出分类搜索新建节点** | 在画布空白处 **单击右键**、按 **`空格 (Space)`** 键，或点顶栏 **`＋ 添加节点 (Space)` (`+ Add Node (Space)`)**（支持顶部分类标签筛选、滚轮翻页、搜中英文名，回车即放） |
| **拖线到空白处自动连线新建** | 从任意输出或未连输入端口拖出贝塞尔曲线，在空白处松开鼠标 → 自动过滤可连节点并在选中后**自动接线** |
| **框选 / 多选移动节点** | 左键空白处拖拽框选；按住 `Shift` 或 `Ctrl` 加选节点一起拖动 |
| **拔插改连 / 一键断线** | 左键按住已连接的输入端口即可像拔插头一样改插到其他输入口或松在空白处丢弃；按住 **`Alt + 左键`** 点击端口/连线或 **`右键单击连线`** 一键秒断 |
| **删除选中节点或连线** | 按 **`Del`** 或 **`Backspace`** |
| **复制选中节点** | **`Ctrl + D`**（自动保留选中子图内部的连线） |
| **居中全部节点** | 按 **`F`** 键（或点顶栏 **居中画布 (`Frame Graph`)**） |
| **撤销 / 重做** | **`Ctrl + Z`** / **`Ctrl + Y`** |

### 3. 暴露材质面板滑块与行内微调参数

1. 在画布中点击选中任意属性节点（`Float`、`Range`、`Boolean`、`Color`、`Vector2`、`Vector3`、`Vector4`），或选中任意带有未连线可调输入端口的运算/程序化节点（如 `Voronoi`、`DissolveEdge`、`Twirl`）。
2. 工具栏下方会出现属性条：
   - 对属性节点可点击 **暴露到材质面板 (`Expose in Inspector`)** 勾选打勾，右侧输入英文参数名（如 `_GlowPower`）按回车（或点回画布自动提交）；
   - 在数值框直接输入默认值（`Range` 节点支持写 `默认值, 最小值, 最大值`，普通运算节点可直接逗号分隔修改未连线的默认输入参数）按回车或失焦自动提交。
3. 切回你的材质 `.material.json`，就能看到暴露出的参数已经自动变成可拖拽的材质滑块或拾色器了！
4. 随时点击顶栏 **打开生成的 HLSL (`View Generated HLSL`)**，还能直接打开节点图编译出的标准 `.fshader.hlsl` 源码对照学习。

### 4. 内置 9 大类 138 个节点速查表（对齐 Unity ASE / Unreal 常用节点库）

连线端口支持自动宽度转换（例如单通道 `Float` 连到 `RGB` 自动广播为 `float3(x, x, x)`，`RGBA` 连到 `RGB` 自动截取 `.xyz`；靠近合法端口约 `20px` 自动磁吸对齐）：

| 分类 | 内置节点（共 138 个） |
| :--- | :--- |
| **1. 几何、相机与光照输入 (`Input`，16 个)** | `UV`、`Time`、`WorldPos`、`LocalPos`、`ObjectPos`（枢轴坐标）、`CameraPos`、`CameraDistance`、`WorldNormal`、`WorldTangent`、`WorldBitangent`、`ViewDir`、`LightDir`（主光方向）、`LightColor`、`ScreenUV`、`Facing`（正背面符号）、`VertexColor` |
| **2. 属性与数学常量 (`Property`，9 个)** | `Float`、`Range`、`Boolean`（开关）、`Color`、`Vector2`、`Vector3`、`Vector4`、`PI`（`PI / TAU / HalfPI / InvPI`）、`GoldenRatio`（`Phi / E / Sqrt2`） |
| **3. 贴图采样与法线处理 (`Texture`，9 个)** | `BaseColorMap`、`NormalMap`、`MetallicRoughnessMap`、`NormalStrength`（法线强度）、`NormalBlend`（RNM 细节法线混合）、`NormalFromHeight`（高度转法线）、`TriplanarBaseColor`（三平面无缝投射）、`ParallaxOffset`（视差偏移 UV）、`Flipbook`（序列帧 UV） |
| **4. UV 坐标与扭曲变换 (`UV`，10 个)** | `TilingOffset`、`Panner`、`Rotator`（旋转）、`PolarCoordinates`（极坐标）、`Twirl`（漩涡）、`RadialShear`（径向切变）、`Spherize`（球面凸镜）、`PixelateUV`（马赛克）、`MirrorUV`（镜像平铺）、`WorldAlignedUV`（世界平面 UV） |
| **5. 数学、插值与波形 (`Math`，41 个)** | 四则与范围：`Add`、`Subtract`、`Multiply`、`Divide`、`MultiplyAdd`、`OneMinus`、`Negate`、`Reciprocal`、`Abs`、`Sign`、`Saturate`、`Clamp`、`Min`、`Max`、`Lerp`、`InverseLerp`、`Remap`、`Smoothstep`、`Step`<br>取整与周期：`Floor`、`Ceil`、`Round`、`Frac`、`Fmod`、`PingPong`<br>幂指对与根号：`Power`、`Sqrt`、`Rsqrt`、`Exp`（`e^x / 2^x`）、`Log`（`ln / log2`）<br>三角与波形：`Sin`、`Cos`、`Tan`、`Asin`、`Acos`、`Atan2`、`Radians`、`Degrees`、`TriangleWave`、`SquareWave`、`SawtoothWave` |
| **6. 向量与通道重组 (`Vector`，14 个)** | `Dot`、`Cross`（叉积）、`Normalize`、`Length`、`Distance`、`Reflect`（反射）、`Project`（投影）、`Reject`（正交平面分量）、`RotateAboutAxis`（绕任意轴旋转）、`Split`、`Combine`、`Append`（追加拼接）、`ComponentMask`（`XY/XZ/YZ/ZYX` 掩码）、`TransformTS2WS`（切线转世界空间） |
| **7. 色彩空间与混合滤镜 (`Color`，14 个)** | `RGBToHSV`、`HSVToRGB`、`HueShift`（色相偏移）、`Saturation`、`Contrast`、`BrightnessGamma`、`Luminance`（感知明度）、`Posterize`（色阶量化）、`ColorRamp`（双色平滑渐变）、`ColorMask`（颜色抠像）、`BlendScreen`（滤色）、`BlendOverlay`（叠加）、`BlendSoftLight`（柔光）、`HDRBoost`（HDR 发光倍增） |
| **8. 程序化噪波与几何图形 (`Procedural`，12 个)** | `SimpleNoise`、`GradientNoise`（柏林梯度噪波）、`FbmNoise`（4 层分形湍流）、`Voronoi`（输出距离与 `CellId`）、`Checkerboard`（棋盘格）、`GridPattern`（网格线）、`BrickPattern`（错缝砖墙）、`EllipseShape`（圆/椭圆）、`RectangleShape`（圆角矩形）、`PolygonShape`（正多边形）、`SphereMask`（3D 球形遮罩）、`WaveRing`（径向涟漪） |
| **9. 风格化光照、特效与分支 (`Artistic`，12 个)** | `Fresnel`、`RimLight`（受光边缘光）、`HalfLambert`（半兰伯特）、`CelShadeBand`（赛璐璐色阶）、`SpecularStylized`（卡通高光）、`MatCapUV`（球面环境捕捉）、`DissolveEdge`（一站式消融灼烧边缘）、`DitherThreshold`（4×4 Bayer 抖动半透明）、`HeightFogMask`（高度渐变）、`VertexWindWave`（植被顶点风吹）、`CompareIf`（条件比较）、`StaticSwitch`（二选一开关） |
| **主表面输出 (`Master`，1 个)** | `MasterSurface`（含 `BaseColor`、`Metallic`、`Roughness`、`NormalTS`、`Emissive`、`Opacity`、`AlphaCutoff` 与驱动顶点局部偏移及实时阴影的 `WorldOffset`） |
