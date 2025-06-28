# CMakeLists.txt za komponentu 'web_server'.
# Definira kako se ova komponenta gradi unutar ESP-IDF projekta.

# Registrira komponentu s ESP-IDF build sustavom.
# Svi parametri unutar zagrada se odnose na konfiguraciju ove komponente.
idf_component_register(
    # SRCS: Specificira izvorne fileove (C, C++, ASM) koji cine ovu komponentu.
    SRCS "web_server.c" "settings.c"

    # INCLUDE_DIRS: Specificira direktorije unutar ove komponente koje ce
    # druge komponente moci ukljuciti koristeci putanju (npr. #include "web_server.h").
    INCLUDE_DIRS "."  "../../main"  # Dodaj ovo ako je "ws2812.h" u "main"


    # REQUIRES: Popis ostalih ESP-IDF komponenti ili eksternih biblioteka
    # od kojih ova komponenta ovisi. Build sustav ce osigurati da su te
    # komponente takodjer buildane i linkane.
    # 'log': za ESP_LOGI, ESP_LOGE, itd.
    # 'esp_http_server': za HTTP server funkcionalnosti.
    # 'fatfs': za rad s FAT datotecnim sustavom (na SD kartici).
    # 'json': za kreiranje i parsiranje JSON podataka  
    REQUIRES nvs_flash log esp_http_server fatfs json  
    

    # EMBED_TXTFILES: Specificira tekstualne fileove koji ce biti ugradjeni
    # u binarni kod komponente kao C nizovi (arrays).
    EMBED_TXTFILES "style.css" "script.js" "chart.js" "index.html" "list.html" "message.html" "logging.html" "settings.html"
)

# Opcionalno: Komentirana linija koja pokazuje kako mozete iskljuciti
# odredjeni warning (format truncation warning) i sprijeciti da se tretira kao greska
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)