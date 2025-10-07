#pragma once

namespace spectre {

enum class StatusCode {
    Ok = 0,
    AlreadyExists,
    NotFound,
    InvalidArgument,
    CapacityExceeded,
    InternalError
};

}
