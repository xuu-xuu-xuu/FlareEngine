using System.Text.Json;

// 已导出的独立游戏可以直接双击。资源与脚本的路径始终相对于包，而非快捷方式的工作目录。
internal static class PackagedLaunch
{
    internal static string[] Arguments(string directory)
    {
        var root=Path.GetFullPath(directory);
        using var doc=JsonDocument.Parse(File.ReadAllText(Path.Combine(root,"launch.json")));
        var launch=doc.RootElement;
        if(launch.GetProperty("schema").GetInt32()!=1)throw new InvalidDataException("Export.InvalidSettings");
        string PathInPackage(string relative)
        {
            if(string.IsNullOrWhiteSpace(relative)||Path.IsPathRooted(relative))throw new InvalidDataException("Export.PathOutsideRoot");
            string full=Path.GetFullPath(Path.Combine(root,relative));
            if(!full.StartsWith(Path.TrimEndingDirectorySeparator(root)+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Export.PathOutsideRoot");
            return full;
        }
        string FilePath(string key)=>PathInPackage(launch.GetProperty(key).GetString()??"");
        var manifest=FilePath("manifest");
        var args=new List<string>{"--window","--project-root",Path.GetDirectoryName(manifest)!,"--player-manifest",manifest,
            "--scene",FilePath("scene"),"--asset-root",FilePath("assetRoot"),"--asset-cache",FilePath("assetCache"),
            "--project",FilePath("scriptAssembly"),"--script",launch.GetProperty("scriptType").GetString()!};
        foreach(var key in new[]{"bindings","ui"})if(launch.TryGetProperty(key,out var value)&&!string.IsNullOrEmpty(value.GetString()))
            args.AddRange(["--"+key,FilePath(key)]);
        if(launch.TryGetProperty("menu",out var menu)&&menu.GetBoolean())args.Add("--menu");
        if(launch.TryGetProperty("locale",out var locale)&&locale.GetString() is "zh-CN" or "en-US")
            args.AddRange(["--locale",locale.GetString()!]);
        return args.ToArray();
    }
}
