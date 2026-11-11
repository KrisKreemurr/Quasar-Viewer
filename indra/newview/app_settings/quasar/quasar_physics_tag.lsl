// Quasar physics tag (doc/quasar_physics_tag.md)
//
// Choose how your own body feels to people using Quasar Viewer: how soft your breasts, butt
// and belly are when an arm presses into them, and how much they wobble afterwards.
//
// HOW TO USE
//  1. Rez a small prim, drop this script in, and WEAR it (any body attachment point, for
//     example Avatar Center or Pelvis -- not a HUD point: HUD objects are only sent to you,
//     so nobody else's viewer would ever see the tag). Make the prim tiny and fully
//     transparent if you like.
//  2. Touch it for the menu, or type in chat on channel 7:
//       /7 soft 0.6      every part's softness, 0 (firm) .. 1 (very soft)
//       /7 breast 0.8    one part: breast, butt or belly, 0 .. 1
//       /7 wobble 1.4    wobble after a press, 0 (none) .. 2 (twice as lively)
//       /7 soft default  stop choosing that value (people's own viewer settings apply)
//       /7 off | /7 on   turn soft contact off / back on for your body
//       /7 show          say the current tag
//       /7 clear         remove the tag
//
// WHAT IT DOES
// It writes a tag like [qlphys soft=0.6 wobble=1.4] into the prim's hover text. Quasar
// Viewer reads it from the objects you wear and never draws it. The simulator does not send
// near-invisible hover text, so the tag is set faintly visible (alpha 0.1): people on other
// viewers may see a faint line of text above you. Nothing else is sent anywhere; the tag
// stays on the prim even after this script is removed.
//
// Full permissions; copy, change and give away freely.

integer CHANNEL = 7;

float soft = -1.0;      // < 0 = not chosen
float wobble = -1.0;
float breast = -1.0;
float butt = -1.0;
float belly = -1.0;
integer off = FALSE;

integer menu_channel;
integer menu_listen;
string picking = "";    // the value a sub-menu is choosing ("" = main menu)

// A number with two decimals, e.g. 0.6 -> "0.60".
string number(float f)
{
    integer cents = llRound(f * 100.0);
    string frac = (string)(cents % 100);
    if (llStringLength(frac) < 2) frac = "0" + frac;
    return (string)(cents / 100) + "." + frac;
}

string tag()
{
    string t = "[qlphys";
    if (off) t += " off";
    if (soft >= 0.0) t += " soft=" + number(soft);
    if (wobble >= 0.0) t += " wobble=" + number(wobble);
    if (breast >= 0.0) t += " breast=" + number(breast);
    if (butt >= 0.0) t += " butt=" + number(butt);
    if (belly >= 0.0) t += " belly=" + number(belly);
    return t + "]";
}

publish()
{
    llSetText(tag(), <1.0, 1.0, 1.0>, 0.1);
}

float clamp(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Set one value by name; value < 0 = not chosen. TRUE if the name is known.
integer set_value(string name, float value)
{
    if (value >= 0.0)
    {
        if (name == "wobble") value = clamp(value, 0.0, 2.0);
        else value = clamp(value, 0.0, 1.0);
    }
    if (name == "soft") soft = value;
    else if (name == "wobble") wobble = value;
    else if (name == "breast") breast = value;
    else if (name == "butt") butt = value;
    else if (name == "belly") belly = value;
    else return FALSE;
    return TRUE;
}

// Read back the tag already on the prim (after a reset or a re-rez).
load()
{
    string text = llList2String(llGetLinkPrimitiveParams(LINK_THIS, [PRIM_TEXT]), 0);
    integer at = llSubStringIndex(llToLower(text), "[qlphys");
    if (at < 0) return;
    string rest = llGetSubString(text, at + 7, -1);
    integer end = llSubStringIndex(rest, "]");
    if (end < 0) return;
    list tokens = llParseString2List(llToLower(llGetSubString(rest, 0, end - 1)), [" "], []);
    integer i;
    for (i = 0; i < llGetListLength(tokens); ++i)
    {
        string token = llList2String(tokens, i);
        if (token == "off") off = TRUE;
        else
        {
            list kv = llParseString2List(token, ["="], []);
            if (llGetListLength(kv) == 2) set_value(llList2String(kv, 0), (float)llList2String(kv, 1));
        }
    }
}

main_menu()
{
    picking = "";
    string state_text = "on";
    if (off) state_text = "OFF";
    llDialog(llGetOwner(), "Quasar physics tag (" + state_text + ")\n" + tag()
        + "\n\nHow your body feels to people on Quasar Viewer.",
        ["Breasts", "Butt", "Belly", "Softness", "Wobble", "On / Off", "Clear", "Done"], menu_channel);
}

value_menu(string name)
{
    picking = name;
    if (name == "wobble")
        llDialog(llGetOwner(), "Wobble after a press:", ["None 0", "Light 0.5", "Normal 1",
            "Lively 1.6", "Wild 2", "Default", "Back"], menu_channel);
    else
        llDialog(llGetOwner(), "Softness (" + name + "):", ["Firm 0.2", "Normal 0.5", "Soft 0.8",
            "Very soft 1", "Default", "Back"], menu_channel);
}

open_menu()
{
    llListenRemove(menu_listen);
    menu_channel = -1000 - (integer)llFrand(1000000000.0);
    menu_listen = llListen(menu_channel, "", llGetOwner(), "");
    llSetTimerEvent(120.0);   // the menu listener closes after two minutes
    main_menu();
}

default
{
    state_entry()
    {
        load();
        publish();
        llListen(CHANNEL, "", llGetOwner(), "");
        if (llGetAttached() == 0)
            llOwnerSay("Wear this object (not as a HUD) so other Quasar Viewer users can read your tag: " + tag());
        else if (llGetAttached() >= ATTACH_HUD_CENTER_2 && llGetAttached() <= ATTACH_HUD_BOTTOM_RIGHT)
            llOwnerSay("This is worn as a HUD: nobody else's viewer receives HUD objects. Wear it on your body instead.");
    }

    on_rez(integer param)
    {
        llResetScript();
    }

    touch_start(integer count)
    {
        if (llDetectedKey(0) == llGetOwner()) open_menu();
    }

    listen(integer channel, string name, key id, string message)
    {
        if (channel == CHANNEL)
        {
            list words = llParseString2List(llToLower(llStringTrim(message, STRING_TRIM)), [" "], []);
            string word = llList2String(words, 0);
            if (word == "off") off = TRUE;
            else if (word == "on") off = FALSE;
            else if (word == "show") { llOwnerSay(tag()); return; }
            else if (word == "clear")
            {
                llSetText("", ZERO_VECTOR, 0.0);
                soft = -1.0; wobble = -1.0; breast = -1.0; butt = -1.0; belly = -1.0; off = FALSE;
                llOwnerSay("Tag removed.");
                return;
            }
            else if (llGetListLength(words) == 2)
            {
                string value = llList2String(words, 1);
                float v = (float)value;
                if (value == "default") v = -1.0;
                if (!set_value(word, v)) { llOwnerSay("Unknown: " + word + " (soft, breast, butt, belly, wobble)"); return; }
            }
            else
            {
                llOwnerSay("Try: /7 soft 0.6, /7 breast 0.8, /7 wobble 1.4, /7 off, /7 on, /7 show, /7 clear");
                return;
            }
            publish();
            llOwnerSay("Tag: " + tag());
            return;
        }

        // the menu
        if (message == "Done") { llListenRemove(menu_listen); llSetTimerEvent(0.0); return; }
        if (message == "Back") { main_menu(); return; }
        if (picking == "")
        {
            if (message == "Breasts") value_menu("breast");
            else if (message == "Butt") value_menu("butt");
            else if (message == "Belly") value_menu("belly");
            else if (message == "Softness") value_menu("soft");
            else if (message == "Wobble") value_menu("wobble");
            else if (message == "On / Off") { off = !off; publish(); main_menu(); }
            else if (message == "Clear")
            {
                soft = -1.0; wobble = -1.0; breast = -1.0; butt = -1.0; belly = -1.0; off = FALSE;
                publish();
                main_menu();
            }
            return;
        }
        float chosen = -1.0;
        if (message != "Default")
        {
            // the number after the label ("Soft 0.8" -> 0.8)
            list parts = llParseString2List(message, [" "], []);
            chosen = (float)llList2String(parts, -1);
        }
        set_value(picking, chosen);
        publish();
        main_menu();
    }

    timer()
    {
        llListenRemove(menu_listen);
        llSetTimerEvent(0.0);
    }
}
