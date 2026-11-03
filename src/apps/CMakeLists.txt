add_executable(chronicles-login-server login_main.cpp postgres_server_lease.cpp)
target_link_libraries(chronicles-login-server PRIVATE
  chronicles-auth
  chronicles-services-login
  chronicles-services-news
  chronicles-services-portal_storage
  PostgreSQL::PostgreSQL
  OpenSSL::Crypto
)
if(WIN32)
  target_link_libraries(chronicles-login-server PRIVATE ws2_32 mswsock)
endif()
chronicles_process_resources(chronicles-login-server server "Project Chronicles Login Server")
