#include <iostream>

int main() {
    int* balance = new int(500);
    std::cout << "Balance: " << *balance << "\n";
    delete balance;
    // ... later, in a different code path someone forgot this was freed
    delete balance;   // double free
    return 0;
}
