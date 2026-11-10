// Exercises the production widget state selection policy without a viewer or GPU.
#include "../llwidgetimage.h"

#include <cassert>
#include <vector>

struct Image { bool ready; };

int main()
{
    Image desired{false}, previous{true}, unselected{true};
    std::vector<Image*> requests;
    auto prepare = [&](Image* image) { requests.push_back(image); return image->ready; };

    assert(ll_choose_widget_image(&desired, &previous, &unselected, prepare) == &previous);
    assert((requests == std::vector<Image*>{&desired, &previous}));
    requests.clear();
    previous.ready = false;
    assert(ll_choose_widget_image(&desired, &previous, &unselected, prepare) == &unselected);
    assert((requests == std::vector<Image*>{&desired, &previous, &unselected}));
    requests.clear();
    unselected.ready = false;
    assert(ll_choose_widget_image(&desired, &previous, &unselected, prepare) == nullptr);
    desired.ready = true;
    requests.clear();
    assert(ll_choose_widget_image(&desired, &previous, &unselected, prepare) == &desired);
    assert((requests == std::vector<Image*>{&desired}));
    desired.ready = false;
    requests.clear();
    assert(ll_choose_widget_image(&desired, &desired, &desired, prepare) == nullptr);
    assert((requests == std::vector<Image*>{&desired}));
    assert(ll_choose_widget_image(static_cast<Image*>(nullptr), static_cast<Image*>(nullptr),
                                  static_cast<Image*>(nullptr), prepare) == nullptr);
}
