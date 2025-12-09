#include "testbackend.hpp"

namespace PelicanStudio {

QString TestBackend::getName() const {
    std::cout << "getName" << std::endl;
    return QString::fromStdString(m_name);
}
void TestBackend::setName(QString new_name) {
    std::cout << "setName" << std::endl;
    m_name = new_name.toStdString();
}

} // namespace PelicanStudio
