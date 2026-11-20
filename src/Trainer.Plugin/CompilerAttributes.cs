// Generated IL2CPP interop assemblies expose compiler attribute stubs without
// their CLR constructors. Local compiler attributes prevent Roslyn binding to
// those stubs while preserving nullable metadata in this real CLR assembly.
namespace System.Runtime.CompilerServices;

[AttributeUsage(AttributeTargets.Class | AttributeTargets.Event | AttributeTargets.Field | AttributeTargets.GenericParameter |
                AttributeTargets.Parameter | AttributeTargets.Property | AttributeTargets.ReturnValue | AttributeTargets.Struct)]
internal sealed class NullableAttribute : Attribute
{
    public readonly byte[] NullableFlags;
    public NullableAttribute(byte value) { NullableFlags = new[] { value }; }
    public NullableAttribute(byte[] value) { NullableFlags = value; }
}

[AttributeUsage(AttributeTargets.Class | AttributeTargets.Delegate | AttributeTargets.Interface | AttributeTargets.Method | AttributeTargets.Struct)]
internal sealed class NullableContextAttribute : Attribute
{
    public readonly byte Flag;
    public NullableContextAttribute(byte value) { Flag = value; }
}
