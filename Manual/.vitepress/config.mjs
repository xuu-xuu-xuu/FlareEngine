import { defineConfig } from 'vitepress'
import { readFileSync } from 'node:fs'

// 只将文档界面的文本键写入站点，避免向浏览器分发整份引擎语言表。
const guide = Object.fromEntries(
  [...readFileSync(new URL('../../Resources/Localization/zh-CN.txt', import.meta.url), 'utf8')
    .matchAll(/^Manual\.Guide\.(\w+)=(.+)$/gm)].map(([, key, value]) => [key, value.trim()])
)

export default defineConfig({
  base: process.env.FLARE_DOCS_BASE || '/',
  lang: 'zh-CN',
  title: 'Flare 开发者文档',
  description: 'Flare 引擎的功能、架构、类型与接口参考',
  appearance: 'force-dark',
  // 两本中文手册按路径使用各自的侧栏和本地搜索索引。
  locales: {
    root: { label: '开发者文档' },
    user: {
      label: '使用者文档',
      lang: 'zh-CN',
      title: 'Flare 使用者文档',
      description: '从创建第一个 Flare 项目、零重启挂载 C# 脚本到运行预览与导出',
      themeConfig: {
        siteTitle: 'Flare 使用者文档',
        sidebar: [
          { text: '使用者文档', link: '/user/' },
          { text: '五步上手第一个项目', items: [
            { text: '新建项目', link: '/user/create-project' },
            { text: '编辑器界面与高效交互', link: '/user/editor-interface' },
            { text: '导入 FBX / GLB', link: '/user/import-fbx' },
            { text: '编写与挂载 C# 脚本', link: '/user/scripting' },
            { text: '运行预览与实时调参', link: '/user/run-preview' }
          ] },
          { text: 'C# API 参考手册', items: [
            { text: 'API 总览与速查导航', link: '/user/api/' },
            { text: '核心：行为、实体、场景与特性', link: '/user/api/core' },
            { text: '输入、相机、时间、数学与异步', link: '/user/api/input-math' },
            { text: '物理、射线、碰撞与角色控制', link: '/user/api/physics' },
            { text: '音频、粒子、导航、UI 与事件', link: '/user/api/systems' }
          ] },
          { text: '材质、布光与画质', items: [
            { text: '自定义 Shader 与 ASE 连线图', link: '/user/custom-shader' },
            { text: '用渲染工作室调光', link: '/user/rendering-studio' },
            { text: '给室内展厅布光', link: '/user/indoor-lighting' },
            { text: '给玩家提供画质设置', link: '/user/player-quality' }
          ] },
          { text: '动画与视频导出', items: [
            { text: '让模型播放骨骼动画', link: '/user/animate-model' },
            { text: '连续抓帧并导出 MP4', link: '/user/record-video' }
          ] },
          { text: '预制体、拖放落场与玩法系统', link: '/user/gameplay-systems' },
          { text: '物理、渲染和资源工具', link: '/user/feature-tools' },
          { text: '从源码启动编辑器', link: '/user/build-from-source' }
        ]
      }
    }
  },
  themeConfig: {
    i18nRouting: false,
    siteTitle: 'Flare 开发者文档',
    guide,
    repository: 'https://github.com/xuu-xuu-xuu/FlareEngine',
    sidebar: [
      { text: '开发者文档', link: '/' },
      { text: '命名规范', link: '/development/命名规范' },
      { text: '渲染与光照', collapsed: true, items: [
        { text: '灯光组件', link: '/runtime/light' },
        { text: '前向管线与帧图', link: '/render/pipeline' },
        { text: '光照着色、材质接口与 Shader 包', link: '/render/materials' },
        { text: '阴影、分簇灯光与环境遮蔽', link: '/render/shadows' },
        { text: '环境光照、大气与反射', link: '/render/environment' },
        { text: 'DXR 1.1 混合硬件光线追踪与 SVGF 降噪', link: '/render/ray-tracing' },
        { text: '后处理、抗锯齿与调色', link: '/render/post' },
        { text: '可见性、Hi-Z 与间接绘制', link: '/render/visibility' },
        { text: 'Mesh Shader 双轨管线与渲染优化', link: '/render/mesh-shader' },
        { text: '确定性粒子模拟与 GPU 绘制', link: '/render/particles' },
        { text: 'LOD、HLOD 与代理几何', link: '/render/detail' },
        { text: 'D3D12 设备、命令与 GPU 资源寿命', link: '/render/device' },
        { text: '交换链、绘制与读回', link: '/render/presentation' }
      ] },
      { text: '世界与运行时', collapsed: true, items: [
        { text: '实体、层级与组件存储', link: '/runtime/entities' },
        { text: '预制体格式、实例化与覆盖同步', link: '/runtime/prefab' },
        { text: '组件基类与生命周期', link: '/runtime/component-base' },
        { text: 'World 的灯光接口', link: '/runtime/world' },
        { text: '原生组件反射与属性序列化', link: '/runtime/metadata' },
        { text: '场景加载、相机与空间查询', link: '/runtime/scenes' },
        { text: '主循环、任务顺序与事件', link: '/runtime/scheduling' },
        { text: '昼夜、天空与场景环境', link: '/runtime/daylight' },
        { text: '刚体、碰撞与角色控制', link: '/runtime/physics' },
        { text: '3D 空间音频与 XAudio2 后端', link: '/runtime/audio' },
        { text: '导航网格烘焙与角色寻路', link: '/runtime/navigation' },
        { text: '骨骼动画与 GPU 蒙皮', link: '/runtime/skeletal-animation' }
      ] },
      { text: '资产与资源', collapsed: true, items: [
        { text: 'FBX 导入', link: '/assets/import' },
        { text: '模型、纹理与材质资产', link: '/assets/models' },
        { text: '文件解码、引用与派生缓存', link: '/assets/storage' },
        { text: '异步加载、上传与纹理驻留', link: '/assets/loading' }
      ] },
      { text: 'C# 脚本与宿主', collapsed: true, items: [
        { text: 'C# 行为、零重启热重载与事件', link: '/scripts/behaviours' },
        { text: '高频游戏 API：变换、时间、输入、数学与异步', link: '/scripts/api' },
        { text: '脚本反射、特性元数据、程序集卸载与 GC', link: '/scripts/lifetime' },
        { text: '托管宿主与原生 ABI', link: '/scripts/bridge' }
      ] },
      { text: 'UI 系统', collapsed: true, items: [
        { text: '自研 UI 单栈架构、文档与布局', link: '/ui/toolkit' },
        { text: '字体图集、文本编辑与 DPI', link: '/ui/text' },
        { text: '控件状态、列表与叠层', link: '/ui/widgets' },
        { text: '游戏 UI、绑定与合成', link: '/ui/game' }
      ] },
      { text: '编辑器', collapsed: true, items: [
        { text: '项目入口、启动进度与编辑器外壳', link: '/editor/workspace' },
        { text: '面板模型、默认布局与界面合成', link: '/editor/panels' },
        { text: '面板拖出与吸附', link: '/editor/docking' },
        { text: '场景视口、变换手柄与选中描边', link: '/editor/viewport' },
        { text: '编辑会话、运行态实时调参与撤销', link: '/editor/session' },
        { text: '资产操作、卡片式属性面板与项目文档', link: '/editor/documents' },
        { text: '功能与默认编辑器的连接', link: '/editor/features' }
      ] },
      { text: '平台与基础服务', collapsed: true, items: [
        { text: '从窗口消息到一帧画面', link: '/platform/frame' },
        { text: '输入、手柄、动作映射与窗口适配', link: '/platform/input' },
        { text: '基础服务、任务与诊断', link: '/platform/services' }
      ] },
      { text: '构建与分发', collapsed: true, items: [
        { text: '构建工具、预览与导出', link: '/tools/delivery' },
        { text: '构建环境、资源复制与包内启动', link: '/tools/package' },
        { text: '固定步连续抓帧与 MP4 编码', link: '/tools/frame-capture' }
      ] }
    ],
    outline: { level: [2, 3], label: '本页内容' },
    sidebarMenuLabel: '目录',
    returnToTopLabel: '回到顶部',
    docFooter: { prev: '上一篇', next: '下一篇' },
    search: {
      provider: 'local',
      options: {
        miniSearch: {
          options: {
            tokenize: (text) => Array.from(
              new Intl.Segmenter('zh-CN', { granularity: 'word' }).segment(text)
            ).filter(part => part.isWordLike).map(part => part.segment)
          }
        },
        translations: {
          button: { buttonText: '搜索文档', buttonAriaLabel: '搜索文档' },
          modal: {
            displayDetails: '显示详情', resetButtonTitle: '清空搜索',
            backButtonTitle: '关闭搜索', noResultsText: '没有找到相关条目',
            footer: { selectText: '打开', navigateText: '切换结果', closeText: '关闭' }
          }
        }
      }
    }
  }
})
