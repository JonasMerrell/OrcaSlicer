#pragma once

#include <optional>
#include <string>

namespace Slic3r {
namespace GUI {

// The link back to rCAD for a model file rCAD sent to the slicer. Next to each file it sends,
// rCAD writes "<file>.rcad.json" naming the document and part the file was made from, the rCAD
// executable, and the local address a running rCAD listens on. The file itself is a plain STEP,
// so everything else about loading and slicing it is unchanged.
struct RcadSource
{
    std::string app;      // rCAD executable
    std::string document; // rCAD document (.json) the part is in
    std::string studio;   // Part Studio name
    std::string part;     // part name
    std::string link;     // host:port of the running rCAD, may be empty
};

// The rCAD source of a model file, if rCAD sent it.
std::optional<RcadSource> rcad_source(const std::string &input_file);

// The rCAD source of the single selected object or part, if rCAD sent it.
std::optional<RcadSource> selected_rcad_source();

// Shows the part in rCAD: asks a running rCAD to show it, or starts rCAD on its document.
// Returns an error message, or an empty string on success.
std::string edit_in_rcad(const RcadSource &source);

} // namespace GUI
} // namespace Slic3r
