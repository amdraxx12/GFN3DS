// Audio : NON IMPLEMENTE dans cette version (le protocole v1 transporte la video et la manette, pas le son).
//
// Ce module existe pour que le reste du programme n'ait pas a le savoir le jour ou l'audio arrivera,
// mais il ne fait rien d'autre que dire la verite : available() renvoie toujours faux.
//
// Ce qu'il faudrait pour l'ajouter (non fait, non teste) :
//   - relais : capturer la sortie audio de Windows (boucle WASAPI), la reechantillonner en 32 kHz stereo
//     16 bits et l'envoyer en UDP (nouveau type de datagramme, numerote pour absorber les pertes) ;
//   - 3DS : ndspInit() exige le fichier sdmc:/3ds/dspfirm.cdc (firmware du DSP, a extraire de SA console
//     avec l'outil DSP1) ; puis ndspChnWaveBufAdd() avec une file de tampons PCM16 ;
//   - synchronisation : l'horloge audio de la 3DS derive par rapport au PC, il faut absorber l'ecart en
//     ajoutant ou supprimant quelques echantillons plutot que de laisser la file grossir (latence).
#pragma once

namespace audio {

bool init();                // sans effet ; renvoie faux (pas d'audio)
void shutdown();
bool available();           // toujours faux
const char* status();       // texte a afficher : "Audio : non implemente"

}  // namespace audio
