using Godot;
using Godot.Collections;

// C# reaching a class of this language by name, with structs as messages
// (spec §11.3): what Root.cs does to Ring's breakout. Used by dotnet_check.gd.
public partial class CsCaller : RefCounted
{
    public Array Found(GodotObject target)
    {
        return new Array
        {
            target.HasMethod("take"),
            target.HasMethod("hidden_take"),
            target.Get("level"),
            target.Get("hidden_level"),
            target.HasSignal("sent"),
            target.HasSignal("hidden_sent"),
        };
    }

    public int Raised(GodotObject target)
    {
        target.Set("level", 5);
        return target.Get("level").AsInt32();
    }

    public Variant Take(GodotObject target, Dictionary hit)
    {
        return target.Call("take", hit);
    }

    public Dictionary Made(GodotObject target)
    {
        return target.Call("made", 4).AsGodotDictionary();
    }

    public Dictionary Heard(GodotObject target)
    {
        Dictionary heard = null;
        target.Connect("sent", Callable.From((Dictionary hit) => heard = hit));
        target.Call("send", 6);
        return heard;
    }
}
