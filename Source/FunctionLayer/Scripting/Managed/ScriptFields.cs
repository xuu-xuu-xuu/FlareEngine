using System.Globalization;
using System.Numerics;
using System.Reflection;
using System.Text.Json;

namespace FlareEngine;

public readonly record struct ExposedField(string Name, string Kind, object? Value,
    string? Header = null, string? Tooltip = null, float? Min = null, float? Max = null,
    float? Space = null, bool ReadOnly = false, string[]? Options = null)
{
    public ExposedField(string Name, string Kind, object? Value) : this(Name, Kind, Value, null, null, null, null, null, false, null) { }
}

public static class ScriptFields
{
    private static bool IsEntityRefType(Type type)
        => type == typeof(EntityReference) || type == typeof(F_EntityReference) || typeof(F_Entity).IsAssignableFrom(type);

    private static ulong ExtractEntityStableId(object? val) => val switch
    {
        EntityReference r => r.StableId,
        F_EntityReference fr => fr.StableId,
        F_Entity ent => ent.IsValid ? ent.StableId : 0,
        _ => 0
    };

    private static object? ConvertEntityFieldValue(Type fieldType, ulong stableId, EngineSession? session)
    {
        if (fieldType == typeof(F_EntityReference)) return new F_EntityReference(stableId);
        if (typeof(F_Entity).IsAssignableFrom(fieldType))
            return stableId == 0 || session is null ? null : session.FindEntity(stableId);
        return new EntityReference(stableId);
    }

    internal static void ClearSceneReferences(F_Behaviour value,IReadOnlySet<ulong> invalidated)
    {
        foreach(var field in Fields(value.GetType()))
        {
            if(!IsEntityRefType(field.FieldType))continue;
            var id=ExtractEntityStableId(field.GetValue(value));
            if(id!=0&&invalidated.Contains(id))
                field.SetValue(value,ConvertEntityFieldValue(field.FieldType,0,null));
        }
    }
    private static readonly JsonSerializerOptions JsonOptions = new() { IncludeFields = true, WriteIndented = true };
    // 每次操作临时枚举，避免长期缓存 FieldInfo 将可卸载项目程序集保活。
    // 字段名是绑定文档的稳定键，继承链上的同名暴露字段必须明确拒绝。
    private static IEnumerable<FieldInfo> Fields(Type type)
    {
        var names = new HashSet<string>(StringComparer.Ordinal);
        for (Type? current = type; current is not null && current != typeof(F_Behaviour) && current != typeof(object); current = current.BaseType)
        {
            foreach (var field in current.GetFields(BindingFlags.Instance | BindingFlags.Public |
                         BindingFlags.NonPublic | BindingFlags.DeclaredOnly).OrderBy(field => field.Name, StringComparer.Ordinal))
            {
                if (!field.IsDefined(typeof(F_ExposeAttribute), inherit: false) || field.IsInitOnly || field.IsStatic) continue;
                if (!names.Add(field.Name)) throw new InvalidOperationException("Script.FieldTypeChanged");
                yield return field;
            }
        }
    }
    private static string? Kind(Type type) => type == typeof(int) ? "i32" : type == typeof(float) ? "f32"
        : type == typeof(double) ? "f64" : type == typeof(bool) ? "bool" : type == typeof(string) ? "string"
        : type == typeof(Vector2) ? "vec2" : type == typeof(Vector3) ? "vec3"
        : type == typeof(F_Color) ? "color" : IsEntityRefType(type) ? "entity"
        : type.IsEnum ? "enum" : null;
    internal static bool TryReadValue(string kind,JsonElement json,out object? value,Type? fieldType=null)
    {
        value=null;
        try
        {
            switch(kind)
            {
                case "i32":value=json.GetInt32();return true;
                case "f32":var single=json.GetSingle();if(!float.IsFinite(single))return false;value=single;return true;
                case "f64":var number=json.GetDouble();if(!double.IsFinite(number))return false;value=number;return true;
                case "bool":value=json.GetBoolean();return true;
                case "string":var text=json.ValueKind==JsonValueKind.Null?null:json.GetString();if(text?.Length>65536)return false;value=text;return true;
                case "vec2":
                    var vec2=new Vector2(json.GetProperty("X").GetSingle(),json.GetProperty("Y").GetSingle());
                    if(!float.IsFinite(vec2.X)||!float.IsFinite(vec2.Y))return false;value=vec2;return true;
                case "vec3":
                    var vector=new Vector3(json.GetProperty("X").GetSingle(),json.GetProperty("Y").GetSingle(),json.GetProperty("Z").GetSingle());
                    if(!float.IsFinite(vector.X)||!float.IsFinite(vector.Y)||!float.IsFinite(vector.Z))return false;value=vector;return true;
                case "color":
                    var cr=json.GetProperty("R").GetSingle();var cg=json.GetProperty("G").GetSingle();var cb=json.GetProperty("B").GetSingle();
                    var ca=json.TryGetProperty("A",out var alphaElem)?alphaElem.GetSingle():1f;
                    if(!float.IsFinite(cr)||!float.IsFinite(cg)||!float.IsFinite(cb)||!float.IsFinite(ca))return false;
                    value=new F_Color(cr,cg,cb,ca);return true;
                case "enum":
                    if(json.ValueKind!=JsonValueKind.String)return false;
                    var enumName=json.GetString();
                    if(string.IsNullOrEmpty(enumName))return false;
                    if(fieldType is { IsEnum: true })
                    {
                        if(!Enum.TryParse(fieldType,enumName,ignoreCase:false,out var enumVal)||!Enum.IsDefined(fieldType,enumVal))return false;
                        value=enumVal;return true;
                    }
                    value=enumName;return true;
                case "entity":
                    if(!ulong.TryParse(json.GetString(),NumberStyles.None,CultureInfo.InvariantCulture,out var reference))return false;value=new EntityReference(reference);return true;
                default:return false;
            }
        }
        catch(Exception exception)when(exception is JsonException or InvalidOperationException or FormatException or OverflowException or KeyNotFoundException or ArgumentException){return false;}
    }
    public static IReadOnlyList<ExposedField> Describe(F_Behaviour behaviour) => Fields(behaviour.GetType())
        .Where(field => Kind(field.FieldType) is not null)
        .Select(field =>
        {
            var header = field.GetCustomAttribute<F_HeaderAttribute>(inherit: false)?.Title;
            var tooltip = field.GetCustomAttribute<F_TooltipAttribute>(inherit: false)?.Text;
            var range = field.GetCustomAttribute<F_RangeAttribute>(inherit: false);
            var space = field.GetCustomAttribute<F_SpaceAttribute>(inherit: false)?.Pixels;
            var readOnly = field.IsDefined(typeof(F_ReadOnlyAttribute), inherit: false);
            var options = field.FieldType.IsEnum ? Enum.GetNames(field.FieldType) : null;
            var k = Kind(field.FieldType)!;
            var rawVal = field.GetValue(behaviour);
            if (k == "entity") rawVal = new EntityReference(ExtractEntityStableId(rawVal));
            return new ExposedField(field.Name, k, rawVal,
                header, tooltip, range?.Min, range?.Max, space, readOnly, options);
        }).ToArray();

    internal static void CopyExposedFields(F_Behaviour source, F_Behaviour target, IReadOnlyDictionary<ulong, ulong>? stableIdRemap = null)
    {
        if (source.GetType() != target.GetType()) return;
        var session = source.Owner?.Session ?? target.Owner?.Session;
        foreach (var field in Fields(source.GetType()))
        {
            if (Kind(field.FieldType) is null) continue;
            var val = field.GetValue(source);
            if (IsEntityRefType(field.FieldType))
            {
                var sid = ExtractEntityStableId(val);
                if (stableIdRemap is not null && sid != 0 && stableIdRemap.TryGetValue(sid, out var mappedId))
                    sid = mappedId;
                val = ConvertEntityFieldValue(field.FieldType, sid, session);
            }
            field.SetValue(target, val);
        }
    }

    internal static void ApplyLive(ScriptSystem system, ulong entityStableId, System.Text.Json.Nodes.JsonObject command)
    {
        var type = command["Type"]?.GetValue<string>() ?? throw new JsonException("Script.TypeMissing");
        var target = system.Attachments.FirstOrDefault(b => b.Entity.IsAlive && b.Entity.StableId == entityStableId && ScriptRegistry.IdOf(b.GetType()) == type)
            ?? throw new InvalidOperationException("Script.InvalidAttachment");
        if (command["Enabled"] is not null)
            target.Enabled = command["Enabled"]!.GetValue<bool>();
        if (command["Field"] is not null)
        {
            var fieldName = command["Field"]!.GetValue<string>();
            var field = Fields(target.GetType()).FirstOrDefault(f => f.Name == fieldName)
                ?? throw new InvalidOperationException("Script.FieldMissing");
            var kind = Kind(field.FieldType) ?? throw new InvalidOperationException("Script.FieldTypeChanged");
            using var doc = JsonDocument.Parse(command["Value"]?.ToJsonString() ?? "null");
            if (!TryReadValue(kind, doc.RootElement, out var val, field.FieldType))
                throw new ArgumentException("Script.FieldValueInvalid");
            if (val is EntityReference entRef)
                val = ConvertEntityFieldValue(field.FieldType, entRef.StableId, system.Session);
            field.SetValue(target, val);
        }
    }

    public static string Save(ScriptSystem system)
    {
        var scripts = system.Attachments.Where(value => value.Entity.IsAlive).Select(value => new SavedScript
        {
            Type = ScriptRegistry.IdOf(value.GetType()),
            Entity = value.Entity.StableId.ToString(CultureInfo.InvariantCulture),
            Enabled = value.Enabled,
            Fields = Fields(value.GetType()).Where(field => Kind(field.FieldType) is not null).ToDictionary(
                field => field.Name,
                field => new SavedField
                {
                    Kind = Kind(field.FieldType)!,
                    Value = IsEntityRefType(field.FieldType)
                        ? JsonSerializer.SerializeToElement(ExtractEntityStableId(field.GetValue(value)).ToString(CultureInfo.InvariantCulture))
                        : field.FieldType.IsEnum
                            ? JsonSerializer.SerializeToElement(field.GetValue(value)?.ToString() ?? "")
                            : JsonSerializer.SerializeToElement(field.GetValue(value), field.FieldType, JsonOptions)
                }, StringComparer.Ordinal)
        }).ToArray();
        return JsonSerializer.Serialize(new SavedDocument { Version = 1, Scripts = scripts }, JsonOptions);
    }

    // 先解析并构建候选列表，再更换附件；损坏 JSON 不清除原有行为配置。
    public static bool Load(ScriptSystem system, string json)
    {
        if (system.IsRunning) throw new InvalidOperationException("Script.LoadWhileRunning");
        SavedDocument? document;
        try
        {
            if (json.Length > 16*1024*1024) throw new JsonException();
            document = JsonSerializer.Deserialize<SavedDocument>(json, JsonOptions);
            if (document is null || document.Version != 1 || document.Scripts is null || document.Scripts.Length > 100000)
                throw new JsonException();
        }
        catch (Exception exception) when (exception is JsonException or NotSupportedException)
        {
            system.Report(null, "Script.DocumentInvalid", exception.GetType().Name);
            return false;
        }
        var candidates = new List<(Entity Entity, F_Behaviour Value)>();
        try
        {
            foreach (var saved in document.Scripts)
            {
                if (saved is null || string.IsNullOrWhiteSpace(saved.Type) || saved.Fields is null
                    || !ulong.TryParse(saved.Entity, NumberStyles.None, CultureInfo.InvariantCulture, out var stableId))
                    throw new JsonException();
                var entity = system.Session.FindEntity(stableId);
                if (entity is null) { system.Report(null, "Script.EntityMissing", saved.Entity); continue; }
                F_Behaviour? value;
                try { value = system.Registry.Create(saved.Type); }
                catch (Exception exception) { system.Report(null, "Script.ConstructionFailed", saved.Type + ": " + exception.GetType().Name); continue; }
                if (value is null) { system.Report(null, "Script.TypeMissing", saved.Type); continue; }
                value.Entity = entity;
                value.EnabledValue = saved.Enabled;
                var fields = Fields(value.GetType()).ToDictionary(field => field.Name, StringComparer.Ordinal);
                foreach (var (name, item) in saved.Fields)
                {
                    if (!fields.TryGetValue(name, out var field)) { system.Report(value, "Script.FieldMissing", name); continue; }
                    if (item is null || Kind(field.FieldType) != item.Kind || item.Kind is null)
                    { system.Report(value, "Script.FieldTypeChanged", name); continue; }
                    try
                    {
                        if(!TryReadValue(item.Kind,item.Value,out var fieldValue,field.FieldType))throw new JsonException();
                        if(fieldValue is EntityReference reference)
                        {
                            if(reference.StableId!=0&&system.Session.FindEntity(reference.StableId) is null)
                                system.Report(value,"Script.ReferenceMissing",name);
                            fieldValue=ConvertEntityFieldValue(field.FieldType,reference.StableId,system.Session);
                        }
                        field.SetValue(value, fieldValue);
                    }
                    catch (Exception exception) when (exception is JsonException or InvalidOperationException or ArgumentException or FormatException)
                    { system.Report(value, "Script.FieldValueInvalid", name); }
                }
                candidates.Add((entity, value));
            }
        }
        catch (JsonException)
        {
            system.Report(null, "Script.DocumentInvalid", "schema");
            return false;
        }
        system.Stop();
        foreach (var (entity, value) in candidates) system.Attach(entity, value);
        return true;
    }

    public static void SaveFile(ScriptSystem system, string path)
    {
        var fullPath = Path.GetFullPath(path);
        var directory = Path.GetDirectoryName(fullPath)!;
        Directory.CreateDirectory(directory);
        var temporary = Path.Combine(directory, Path.GetFileName(path) + "." + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            File.WriteAllText(temporary, Save(system), new System.Text.UTF8Encoding(false));
            File.Move(temporary, fullPath, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    private sealed class SavedDocument { public int Version { get; set; } public SavedScript[]? Scripts { get; set; } }
    private sealed class SavedScript
    {
        public string Type { get; set; } = "";
        public string Entity { get; set; } = "";
        public bool Enabled { get; set; }
        public Dictionary<string,SavedField>? Fields { get; set; }
    }
    private sealed class SavedField { public string? Kind { get; set; } public JsonElement Value { get; set; } }
}
