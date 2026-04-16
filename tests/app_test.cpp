#include <cassert>

#include "mywebserver/app.h"

int main() {
    assert(mywebserver::greeting() == "Hello from mywebserver");
    return 0;
}
