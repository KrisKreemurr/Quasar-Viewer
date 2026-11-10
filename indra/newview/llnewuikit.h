// #703 / #1242 The new interface's widget kit: theme tokens and the component stylesheet.
//
// Every screen of the new interface is an RML document styled only by this kit's classes
// (bar, btn, field, chip, panel, popover, list / row, kbd, text styles). Colours, radii,
// spacing and type come from a token table, so a theme is a table: dark, light and glass
// today; the classic look (a classic skin and layout over the same kit) is another table plus
// its own layout documents later. The stylesheet is generated from the table at load, so a
// theme switch reloads one stylesheet; documents and controllers stay.
#pragma once

#include <RmlUi/Core/DataModelHandle.h>
#include <boost/signals2/connection.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
class DataModelConstructor;
}

// Binds gSavedSettings controls to data variables of one RmlUi data model: the variable reads and
// writes the setting, and a change of the setting (from anywhere) dirties the variable. The
// binding owns its signal connections; destroy it with the model. Bounded by its bindings.
class LLNewUISettingBinding
{
public:
    explicit LLNewUISettingBinding(Rml::DataModelHandle model) : mModel(model) {}
    // Boolean / number / string settings by control name; false when the control does not exist.
    bool bind(Rml::DataModelConstructor& model, const std::string& variable, const std::string& control);

private:
    Rml::DataModelHandle                                   mModel;
    std::vector<boost::signals2::scoped_connection>        mConnections;
};

class LLNewUIKit
{
public:
    enum class Theme
    {
        Dark,
        Light
    };
    // Token name -> RCSS value ("surface" -> "#16181dff").
    using Tokens = std::vector<std::pair<std::string, std::string>>;
    static Tokens tokens(Theme theme, bool glass);
    // The kit stylesheet with every $token replaced (unknown tokens are left visible as-is so a
    // missing one shows in review instead of silently defaulting).
    static std::string stylesheet(const Tokens& tokens);
    // Registers the kit's faces (Inter, with DejaVu as the fallback for missing glyphs).
    static void loadFonts();
};
