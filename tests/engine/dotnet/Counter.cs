using Godot;

// A C# class as this language sees it through reflection (spec §11.1), used
// by cs_view.pet. The editor lists only exported members of C# classes.
[GlobalClass]
public partial class Counter : Node
{
    public const int Limit = 10;

    [Signal]
    public delegate void BumpedEventHandler(int total);

    [Export]
    public int Count { get; set; }

    [Export]
    public Node Target { get; set; }

    [Export]
    public Variant Anything { get; set; }

    public Counter()
    {
        Count = 1;
    }

    public int Add(int n)
    {
        Count += n;
        EmitSignal(SignalName.Bumped, Count);
        return Count;
    }

    public Counter Twin(bool made)
    {
        return made ? new Special() : null;
    }
}
