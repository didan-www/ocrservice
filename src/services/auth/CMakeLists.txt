ocr_add_module(
    NAME auth_service
    SOURCES
        AuthService.cpp
        AuthTypes.cpp
        SessionStore.cpp
    PUBLIC_DEPENDENCIES
        domain
        security
    PRIVATE_DEPENDENCIES
        serialization_time
        text
)
