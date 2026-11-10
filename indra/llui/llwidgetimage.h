/** Choose a drawable widget state image while requesting the desired one. */
#ifndef LL_LLWIDGETIMAGE_H
#define LL_LLWIDGETIMAGE_H

template <typename Image, typename Prepare>
Image* ll_choose_widget_image(Image* desired, Image* previous, Image* unselected, Prepare prepare)
{
    if (desired && prepare(desired)) return desired;
    if (previous && previous != desired && prepare(previous)) return previous;
    if (unselected && unselected != desired && unselected != previous && prepare(unselected)) return unselected;
    return nullptr;
}

#endif
