// Interface-ID definitions the SDK's vstinitiids.cpp does not cover: the
// plug-view GUI interfaces live in pluginterfaces/gui and their IIDs are
// normally emitted by whichever host TU opts in. Our PlugFrame implements
// IPlugFrame, whose queryInterface compares against IPlugFrame::iid.
#include "pluginterfaces/gui/iplugview.h"

namespace Steinberg {
DEF_CLASS_IID(IPlugFrame)
DEF_CLASS_IID(IPlugView)
}  // namespace Steinberg
