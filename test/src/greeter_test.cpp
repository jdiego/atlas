
#include "atlas/greeter.hpp"
#include "atlas/version.hpp"
#include <iostream>
#include <string>
#include <boost/ut.hpp>

namespace ut = boost::ut;


ut::suite<"AtlasTestSuite"> greeter_suite = [] {
    using namespace ut;
    using namespace atlas;
    "greeter"_test = [] {
        Atlas greeter("Tests");
        expect(greeter.greet(LanguageCode::EN) == "Hello, Tests!");
        expect(greeter.greet(LanguageCode::DE) == "Hallo Tests!");
        expect(greeter.greet(LanguageCode::ES) == "¡Hola Tests!");
        expect(greeter.greet(LanguageCode::FR) == "Bonjour Tests!");
    };

    "version"_test = [] {
        expect(std::string_view(ATLAS_VERSION) == std::string_view("1.0.0") >> fatal);
        expect(std::string(ATLAS_VERSION) == std::string("1.0.0"));
    };
};
