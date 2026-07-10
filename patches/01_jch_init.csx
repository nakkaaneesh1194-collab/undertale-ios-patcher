// Patch 1: Fix j_ch initialization in obj_time Create event
// On iOS with Butterscotch, SDL2 puts the virtual controller on slot 0 (device index 0),
// so j_ch should be 1 (GML is 1-indexed). Skip the gamepad_is_connected() loop since
// that function is unreliable on iOS with the stock runner.

EnsureDataLoaded();
GlobalDecompileContext ctx = new(Data);
var dSettings = new Underanalyzer.Decompiler.DecompileSettings();
var importGroup = new UndertaleModLib.Compiler.CodeImportGroup(Data, ctx, dSettings);

importGroup.QueueFindReplace("gml_Object_obj_time_Create_0",
@"if (global.osflavor >= 4)
{
    j_ch = 1;
    for (var i = 0; i < gamepad_get_device_count(); i++)
    {
        if (gamepad_is_connected(i))
        {
            j_ch = i + 1;
        }
    }
}",
@"if (global.osflavor >= 4)
{
    j_ch = 1;
}");

importGroup.Import();
ScriptMessage("Patch 1 done: j_ch init fixed");
