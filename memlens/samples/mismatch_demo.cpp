#include <iostream>

int main() {
    int* buffer = new int[64];
    std::cout << "Allocated a 64-int buffer\n";
    delete buffer;   // should be delete[] buffer
    return 0;
}
