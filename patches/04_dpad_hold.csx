// Patch 4: D-pad / joystick hold fix
// keyboard_key_press() was only called on the rising edge (j_fr != j_fr_p && j_fr == 1),
// meaning you had to tap repeatedly to move. Fix: call it every frame while held (j_fr == 1).
// keyboard_key_release() is unchanged — still fires on falling edge only.

EnsureDataLoaded();
GlobalDecompileContext ctx = new(Data);
var dSettings = new Underanalyzer.Decompiler.DecompileSettings();
var importGroup = new UndertaleModLib.Compiler.CodeImportGroup(Data, ctx, dSettings);

importGroup.QueueFindReplace("gml_Object_obj_time_Step_1",
@"    if (j_fr != j_fr_p && j_fr == 1)
    {
        keyboard_key_press(vk_right);
    }
    if (j_fl != j_fl_p && j_fl == 1)
    {
        keyboard_key_press(vk_left);
    }
    if (j_fd != j_fd_p && j_fd == 1)
    {
        keyboard_key_press(vk_down);
    }
    if (j_fu != j_fu_p && j_fu == 1)
    {
        keyboard_key_press(vk_up);
    }
    if (j_fr != j_fr_p && j_fr == 0)
    {
        keyboard_key_release(vk_right);
    }
    if (j_fl != j_fl_p && j_fl == 0)
    {
        keyboard_key_release(vk_left);
    }
    if (j_fd != j_fd_p && j_fd == 0)
    {
        keyboard_key_release(vk_down);
    }
    if (j_fu != j_fu_p && j_fu == 0)
    {
        keyboard_key_release(vk_up);
    }",
@"    if (j_fr == 1)
    {
        keyboard_key_press(vk_right);
    }
    if (j_fl == 1)
    {
        keyboard_key_press(vk_left);
    }
    if (j_fd == 1)
    {
        keyboard_key_press(vk_down);
    }
    if (j_fu == 1)
    {
        keyboard_key_press(vk_up);
    }
    if (j_fr != j_fr_p && j_fr == 0)
    {
        keyboard_key_release(vk_right);
    }
    if (j_fl != j_fl_p && j_fl == 0)
    {
        keyboard_key_release(vk_left);
    }
    if (j_fd != j_fd_p && j_fd == 0)
    {
        keyboard_key_release(vk_down);
    }
    if (j_fu != j_fu_p && j_fu == 0)
    {
        keyboard_key_release(vk_up);
    }");

importGroup.Import();
ScriptMessage("Patch 4 done: d-pad hold fixed");
