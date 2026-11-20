using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
#if GAME_INTEROP
using Il2CppInterop.Common;
using Il2CppInterop.Runtime;
using Il2CppInterop.Runtime.Runtime;
#endif

namespace Trainer.Plugin;

/// <summary>Read-only observations. Native offsets come from this process's IL2CPP metadata.</summary>
internal sealed class NativeTelemetry
{
    private readonly DiagnosticStore _diagnostics;
    private readonly Dictionary<(IntPtr Class, string Field), NativeField> _fields = new();
    private sealed record NativeField(IntPtr Info, uint Offset, string Type, string Owner);
    internal NativeTelemetry(DiagnosticStore diagnostics) { _diagnostics = diagnostics; }

    internal void SampleFloat(string key, IntPtr owner, string nativeField)
    {
#if GAME_INTEROP
        try
        {
            var field = Resolve(owner, nativeField);
            if (field.Type != "System.Single") throw new InvalidDataException("Expected System.Single, got " + field.Type);
            var address = IntPtr.Add(owner, checked((int)field.Offset));
            Describe(key, owner, nativeField, field, address);
            _diagnostics[key + ".value"] = ReadFloat(address).ToString("R", CultureInfo.InvariantCulture);
        }
        catch (Exception e) { ClearValues(key); _diagnostics[key + ".error"] = e.Message; }
#endif
    }

    internal void SampleFloat2(string key, IntPtr owner, string nativeField)
    {
#if GAME_INTEROP
        try
        {
            var field = Resolve(owner, nativeField);
            if (field.Type != "Unity.Mathematics.float2") throw new InvalidDataException("Expected Unity.Mathematics.float2, got " + field.Type);
            var address = IntPtr.Add(owner, checked((int)field.Offset));
            Describe(key, owner, nativeField, field, address);
            _diagnostics[key + ".x.address"] = Hex(address);
            _diagnostics[key + ".y.address"] = Hex(IntPtr.Add(address, 4));
            _diagnostics[key + ".x.value"] = ReadFloat(address).ToString("R", CultureInfo.InvariantCulture);
            _diagnostics[key + ".y.value"] = ReadFloat(IntPtr.Add(address, 4)).ToString("R", CultureInfo.InvariantCulture);
        }
        catch (Exception e) { ClearValues(key); _diagnostics[key + ".error"] = e.Message; }
#endif
    }

    internal unsafe void DescribeMethod(MethodInfo method, string phase)
    {
#if GAME_INTEROP
        var key = "native." + method.DeclaringType!.FullName + "." + method.Name + "(" +
                  string.Join(",", method.GetParameters().Select(p => p.ParameterType.Name)) + ")";
        try
        {
            var generatedField = Il2CppInteropUtils.GetIl2CppMethodInfoPointerFieldForGeneratedMethod(method)
                ?? throw new MissingFieldException("Generated NativeMethodInfoPtr field not found.");
            var infoPointer = (IntPtr)(generatedField.GetValue(null) ?? IntPtr.Zero);
            if (infoPointer == IntPtr.Zero) throw new InvalidDataException("Native MethodInfo pointer is null.");
            var info = UnityVersionHandler.Wrap((Il2CppMethodInfo*)infoPointer);
            if (info == null) throw new InvalidDataException("No version-specific native MethodInfo handler.");
            _diagnostics[key + ".methodInfo"] = Hex(infoPointer);
            _diagnostics[key + ".generatedField"] = generatedField.Name;
            _diagnostics[key + ".entry." + phase] = Hex(info.MethodPointer);
            _diagnostics[key + ".addressSource"] = "Generated method IL -> NativeMethodInfoPtr -> UnityVersionHandler.MethodPointer";
        }
        catch (Exception e) { _diagnostics[key + ".error"] = e.Message; }
#endif
    }

    internal void ClearValues(string prefix) => _diagnostics.ClearPrefix(prefix);

#if GAME_INTEROP
    private NativeField Resolve(IntPtr owner, string name)
    {
        if (owner == IntPtr.Zero) throw new InvalidDataException("Native object is unavailable.");
        var klass = IL2CPP.il2cpp_object_get_class(owner);
        if (klass == IntPtr.Zero) throw new InvalidDataException("Native object class is unavailable.");
        if (_fields.TryGetValue((klass, name), out var cached)) return cached;
        IntPtr declaring = klass;
        for (int depth = 0; declaring != IntPtr.Zero && depth < 64; depth++, declaring = IL2CPP.il2cpp_class_get_parent(declaring))
        {
            var info = IL2CPP.il2cpp_class_get_field_from_name(declaring, name);
            if (info == IntPtr.Zero) continue;
            if ((IL2CPP.il2cpp_field_get_flags(info) & 0x10) != 0) throw new InvalidDataException("Static fields cannot use object-relative addresses.");
            uint offset = IL2CPP.il2cpp_field_get_offset(info);
            if (offset < IntPtr.Size * 2 || offset > 1024 * 1024) throw new InvalidDataException("Native field offset is outside an expected object layout.");
            var type = IL2CPP.il2cpp_type_get_name_(IL2CPP.il2cpp_field_get_type(info))
                ?? throw new InvalidDataException("Native field type is unavailable.");
            var actualDeclaring = IL2CPP.il2cpp_field_get_parent(info);
            string ownerName = IL2CPP.il2cpp_class_get_namespace_(actualDeclaring) + "." + IL2CPP.il2cpp_class_get_name_(actualDeclaring);
            var result = new NativeField(info, offset, type, ownerName.TrimStart('.'));
            _fields[(klass, name)] = result;
            return result;
        }
        throw new MissingFieldException("Native field not found: " + name);
    }

    private void Describe(string key, IntPtr owner, string name, NativeField field, IntPtr address)
    {
        _diagnostics.Remove(key + ".error");
        _diagnostics[key + ".object"] = Hex(owner);
        _diagnostics[key + ".field"] = name;
        _diagnostics[key + ".declaringType"] = field.Owner;
        _diagnostics[key + ".fieldInfo"] = Hex(field.Info);
        _diagnostics[key + ".offset"] = "0x" + field.Offset.ToString("X", CultureInfo.InvariantCulture);
        _diagnostics[key + ".type"] = field.Type;
        _diagnostics[key + ".address"] = Hex(address);
        _diagnostics[key + ".source"] = "il2cpp_class_get_field_from_name + il2cpp_field_get_offset; no writes";
    }

    private static float ReadFloat(IntPtr address) => BitConverter.Int32BitsToSingle(Marshal.ReadInt32(address));
#endif
    internal static string Hex(IntPtr address) => "0x" + address.ToInt64().ToString("X", CultureInfo.InvariantCulture);
}
