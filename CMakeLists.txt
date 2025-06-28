# CMakeLists.txt - Glavni CMake file za ESP-IDF projekt

# Minimalna zahtijevana verzija CMake-a za ovaj projekt.
cmake_minimum_required(VERSION 3.5)

# Ukljucuje standardni ESP-IDF build sustav i sve potrebne funkcionalnosti.
# Ovo je kljucni korak koji inicijalizira build okruzenje.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)



# Definira ime projekta. Ovo ime se koristi u raznim build koracima i u outputu (npr. ime .bin filea).
# Ime u zagradama 'sd_card_web_8final' je obicno isto kao i ime glavnog projektnog foldera.
project(sd_card_web_server5_ads_settings)