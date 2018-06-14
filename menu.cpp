#include "menu.h"



int main(int argc, char *argv[]) {
    next_radio_receiver r;
    if (r.init(argc, argv))
        return 1;
    r.work();

    return 0;
}
