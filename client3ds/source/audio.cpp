// Audio : voir audio.hpp. Rien n'est joue ; ce fichier ne pretend pas le contraire.
#include "audio.hpp"

namespace audio {

bool init() { return false; }
void shutdown() {}
bool available() { return false; }
const char* status() { return "Audio : non implemente (video et manette seulement)"; }

}  // namespace audio
