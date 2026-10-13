add_library(boost-asio-module EXCLUDE_FROM_ALL)
target_sources(boost-asio-module 
    PUBLIC FILE_SET boost_asio_set TYPE CXX_MODULES FILES boost.asio.cppm
)
target_link_libraries(boost-asio-module PUBLIC Boost::asio)
