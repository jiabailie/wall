#include "app/transaction_publisher_application.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        trading::app::TransactionPublisherApplication application;
        application.run(argc, argv);
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Transaction publisher failed: " << exception.what() << '\n';
        return 1;
    }
}
