#ifndef GREETER_HPP
#define GREETER_HPP

#include <string>

namespace atlas {

/**  Language codes to be used with the Atlas class */
enum class LanguageCode { EN, DE, ES, FR };

/**
 * @brief A class for saying hello in multiple languages
 */
class Atlas {
  public:
    /**
     * @brief Creates a new greeter
     * @param name the name to greet
     */
    Atlas(std::string name);
    /**
     * @brief Creates a localized string containing the greeting
     * @param lang the language to greet in
     * @return a string containing the greeting
     */
    std::string greet(LanguageCode lang = LanguageCode::EN) const;

  private:
    std::string name;
};

} // namespace atlas
#endif