// Build switch for the Restwise watch firmware.
//
// Leave the line below commented out for the full build (WiFi app, Discord, AI Chatbot, NTP, Dino ...).
// tools/build_variants.py turns it on in a temporary copy of the sketch to make the NO-RADIO build:
// no WiFi, no Bluetooth, no Discord, no AI Chatbot, no NTP -- the code is not even compiled in.
//
// #define RESTWISE_NO_RADIO
