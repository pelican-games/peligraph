#pragma once
#include <stdexcept>
namespace Pelican {
// Only authored data failures carry this mark. Internal invariants and GPU
// errors deliberately retain their original exception types.
class UserDataError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
}
