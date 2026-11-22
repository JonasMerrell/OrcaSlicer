#ifndef slic3r_GUI_GCodeHtmlExport_hpp_
#define slic3r_GUI_GCodeHtmlExport_hpp_

// Fork: export the G-code preview as one self-contained HTML file.
//
// The toolpaths the preview shows (every move, with its feature, width, height, speed, fan,
// temperature and time) are packed column by column, compressed and embedded in a copy of
// resources/web/gcode_html/viewer.html, a small WebGL viewer that needs no network access.

#include <array>
#include <string>
#include <vector>

namespace libvgcode {
class Viewer;
}

namespace Slic3r {
namespace GUI {

struct GCodeHtmlInfo
{
    std::string title;     // project or file name
    std::string printer;   // printer preset name
    std::string generator; // "OrcaSlicer 2.x"
    // Subtracted from the preview positions, so the file shows plate coordinates.
    double origin_x = 0.0;
    double origin_y = 0.0;
    std::vector<std::array<double, 2>> bed; // printable area outline, plate coordinates
    double bed_height = 0.0;
    std::vector<std::string> filaments;     // filament preset name per extruder
};

// Writes the HTML file. Returns false and sets error when it could not.
bool export_gcode_html(const libvgcode::Viewer& viewer,
                       const GCodeHtmlInfo&     info,
                       const std::string&       template_path,
                       const std::string&       filename,
                       std::string&             error);

} // namespace GUI
} // namespace Slic3r

#endif
