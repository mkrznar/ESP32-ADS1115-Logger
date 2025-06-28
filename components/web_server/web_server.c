// components/web_server/web_server.c

// Implementacija HTTP web servera za ESP32 koristeći ESP-IDF httpd komponentu.
// Server omogućava posluživanje statičkih web stranica (ugrađenih u firmware),
// prikazivanje popisa datoteka na SD kartici, preuzimanje i brisanje datoteka,
// te upload novih datoteka na SD karticu. Također pruža API za status logiranja
// i čitanje ADS1115 vrijednosti preko HTTP zahtjeva.

// --- Uključivanje standardnih C/C++ biblioteka ---
#include <stdio.h>      // Standardni ulaz/izlaz funkcije (npr. printf, snprintf, fopen, fread, fwrite, fclose)
#include <string.h>     // Funkcije za rad sa stringovima (npr. strlen, strcpy, strncpy, strstr, strcmp, strcasecmp)
#include <sys/unistd.h> // Standardne POSIX funkcije (npr. unlink za brisanje datoteka, stat za dohvat informacija o fileu)
#include <sys/stat.h>   // Strukture i funkcije za dohvat informacija o statusu filea/direktorija (stat)
#include <dirent.h>     // Funkcije za rad s direktorijima (npr. opendir, readdir, closedir)
#include <errno.h>      // Definicija globalne varijable errno za indikaciju grešaka sustavnih poziva
#include <stdlib.h>     // Standardne C funkcije (npr. malloc, free, realloc za dinamičku alokaciju memorije)
#include <ctype.h>      // Za funkcije provjere tipa znakova (npr. isxdigit za provjeru je li znak heksadecimalna znamenka)
#include <sys/param.h>  // Za MIN makro (dolazi s ESP-IDF ili standardnim C/C++ knjižnicama), koristi se za ograničavanje veličine čitanja/pisanja

// --- Uključivanje ESP-IDF specifičnih headera ---
#include "esp_log.h"         // ESP32 logging framework - za ispis poruka na konzolu
#include "esp_http_server.h" // ESP-IDF komponenta za implementaciju HTTP/1.1 web servera
#include "esp_err.h"         // Standardni ESP32 tip za greške i makroi (npr. ESP_OK, ESP_FAIL, esp_err_to_name)
#include "esp_vfs_fat.h"     // Komponenta Virtual File System (VFS) za FATFS - omogućava rad sa SD karticom kao standardnim file sustavom

// --- Uključivanje specifičnih projektnih headera ---
#include "cJSON.h"             // Biblioteka za parsiranje i generiranje JSON formata podataka (koristi se za AJAX odgovore)
#include "freertos/FreeRTOS.h" // FreeRTOS baza, potrebna za korištenje mutexa
#include "freertos/semphr.h"   // FreeRTOS Semaphores, ovdje specifično za Mutex
#include <stdbool.h>           // Standardni tip za boolean vrijednosti (true/false)
#include "settings.h"          // Header za funkcije upravljanja postavkama (vjerojatno pohranjenim u NVS ili drugdje)
#include "ws2812.h"            // Header za WS2812 LED driver (iako se funkcije iz njega ne koriste direktno ovdje, uključen je)

// --- Globalne varijable i mutex za sinkronizaciju ---

// Varijabla koja prati status logiranja (aktivno/neaktivno). 'static' ograničava opseg na ovu datoteku.
static bool logging_active = false;
// Mutex za osiguravanje thread-safe pristupa globalnim varijablama 'logging_active' i 'last_voltages'.
// Više taskova (npr. web server handler i task za očitavanje ADC-a) mogu pokušati pristupiti ovim varijablama istovremeno.
static SemaphoreHandle_t logging_mutex = NULL;
// Polje za pohranu zadnje očitanih naponskih vrijednosti s dva ADS1115 modula, ukupno 8 kanala.
static float last_voltages[8] = {0};

// Extern deklaracije za globalne varijable iz main.c
// Ove varijable čuvaju putanju do trenutne log datoteke i mutex za pristup njoj.
extern char g_current_log_filepath[]; // Definirano u main.c
extern SemaphoreHandle_t g_log_file_path_mutex; // Definirano u main.c


// --- Funkcije za upravljanje globalnim stanjem ---

// Funkcija: set_last_voltages
// Opis: Ažurira globalne varijable 'last_voltages' s novim očitanim vrijednostima.
//       Koristi mutex za siguran pristup u multi-thread okruženju.
// Argumenti:
//   - voltages: Pokazivač na niz float vrijednosti koje predstavljaju zadnje očitanja s ADC-a.
void set_last_voltages(const float *voltages)
{
    // Provjeri jesu li mutex i ulazni niz validni
    if (logging_mutex && voltages)
    {
        // Pokušaj preuzeti mutex s timeoutom od 10ms.
        // Ako se mutex uspješno preuzme (nitko drugi ga ne koristi trenutno), nastavi.
        if (xSemaphoreTake(logging_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            // Kopiraj ulazne vrijednosti u globalnu varijablu 'last_voltages'.
            memcpy(last_voltages, voltages, sizeof(last_voltages));
            // Otpusti mutex kako bi ga drugi taskovi mogli preuzeti.
            xSemaphoreGive(logging_mutex);
        }
        // Ako mutex nije uspješno preuzet unutar 10ms, preskače se ažuriranje.
        // Ovo sprječava blokiranje web server handlera ako je task za logiranje zauzet.
    }
}

// Funkcija: set_logging_active
// Opis: Postavlja status logiranja (aktivno/neaktivno).
//       Osigurava da je mutex inicijaliziran i koristi ga za thread-safe postavljanje statusa.
// Argumenti:
//   - active: Boolean vrijednost (true ili false) kojom se postavlja status logiranja.
void set_logging_active(bool active)
{
    // Provjeri je li mutex inicijaliziran. Ako nije, kreiraj ga.
    if (logging_mutex == NULL)
    {
        logging_mutex = xSemaphoreCreateMutex();
    }
    // Pokušaj preuzeti mutex i čekaj beskonačno (portMAX_DELAY) dok ne postane dostupan.
    // Oprez: Beskonačno čekanje može uzrokovati zaglavljivanje (deadlock) ako mutex nikad ne postane dostupan.
    // U kritičnim putevima bolje koristiti timeout. Ovdje se pretpostavlja da se ovo ne poziva u jako kritičnom dijelu.
    if (xSemaphoreTake(logging_mutex, portMAX_DELAY))
    {
        // Ako je mutex preuzet, sigurno postavi globalnu varijablu.
        logging_active = active;
        // Otpusti mutex.
        xSemaphoreGive(logging_mutex);
    }
}

// Funkcija: is_logging_enabled
// Opis: Vraća trenutni status logiranja.
//       Osigurava thread-safe čitanje globalne varijable 'logging_active'.
// Povratna vrijednost: bool - true ako je logiranje aktivno, false inače.
bool is_logging_enabled(void)
{
    bool status;
    // Provjeri je li mutex inicijaliziran. Ako nije, kreiraj ga.
    if (logging_mutex == NULL)
    {
        logging_mutex = xSemaphoreCreateMutex();
    }
    // Pokušaj preuzeti mutex i čekaj beskonačno.
    if (xSemaphoreTake(logging_mutex, portMAX_DELAY))
    {
        // Ako je mutex preuzet, sigurno pročitaj globalnu varijablu.
        status = logging_active;
        // Otpusti mutex.
        xSemaphoreGive(logging_mutex);
    }
    else
    {
        // Ako mutex nije preuzet (ovo bi se trebalo dogoditi samo u slučaju interne FreeRTOS greške ili deadlocka),
        // vrati false. Logiranje greške ovdje nedostaje, moglo bi se dodati ako je kritično.
        status = false;
    }
    // Vrati pročitani status.
    return status;
}

// --- Definicije konstanti i makroa ---

// Definicija tocke montiranja za SD karticu na virtualnom file sustavu ESP32.
// Ova vrijednost mora odgovarati onoj koja je korištena pri inicijalizaciji SD kartice u main.c.
#define MOUNT_POINT "/sdcard"

// TAG za logiranje specifičan za web server komponentu. Koristi se u ESP_LOG* makroima za filtriranje ispisa.
static const char *TAG_WEB = "web_server";
// Handler za HTTP server instancu. Ova varijabla pohranjuje referencu na pokrenuti HTTP server.
// Koristi se za konfiguraciju, pokretanje, zaustavljanje i registraciju URI handlera.
static httpd_handle_t server = NULL;

// Definicije konstanti vezanih za funkcionalnost uploada datoteka.
#define UPLOAD_BUFFER_SIZE 2048 // Veličina privremenog buffera koji se koristi za čitanje dijelova (chunkova) uploadane datoteke iz HTTP zahtjeva.
#define FILE_PATH_MAX 256       // Maksimalna dopuštena duljina pune putanje do datoteke (uključujući točku montiranja, '/'). Koristi se za sprečavanje prelijevanja buffera.

// --- Extern deklaracije za binarne podatke ugrađenih fileova ---
// Ovi nizovi bajtova predstavljaju sadržaj statičkih web fileova (CSS, JS, HTML)
// koji su ugrađeni (embedded) direktno u firmware prilikom buildanja.
// To se obično postiže pomoću ESP-IDF CMake funkcionalnosti (npr. target_add_binary_data).
// '_binary_%s_start' i '_binary_%s_end' konvencija imenovanja se koristi
// za označavanje početka i kraja binarnog niza u RAM-u.
extern const unsigned char style_css_start[] asm("_binary_style_css_start");
extern const unsigned char style_css_end[] asm("_binary_style_css_end");
extern const unsigned char script_js_start[] asm("_binary_script_js_start");
extern const unsigned char script_js_end[] asm("_binary_script_js_end");
extern const unsigned char chart_js_start[] asm("_binary_chart_js_start");
extern const unsigned char chart_js_end[] asm("_binary_chart_js_end");
extern const unsigned char index_html_start[] asm("_binary_index_html_start");
extern const unsigned char index_html_end[] asm("_binary_index_html_end");
extern const unsigned char list_html_start[] asm("_binary_list_html_start");
extern const unsigned char list_html_end[] asm("_binary_list_html_end");
extern const unsigned char message_html_start[] asm("_binary_message_html_start");
extern const unsigned char message_html_end[] asm("_binary_message_html_end");
extern const unsigned char logging_html_start[] asm("_binary_logging_html_start");
extern const unsigned char logging_html_end[] asm("_binary_logging_html_end");
extern const unsigned char settings_html_start[] asm("_binary_settings_html_start");
extern const unsigned char settings_html_end[] asm("_binary_settings_html_end");

// --- Pomoćne funkcije ---
// Ove funkcije se koriste interno unutar web server modula kako bi se izbjeglo ponavljanje koda.

// Funkcija: build_filepath
// Opis: Pomoćna funkcija za sigurnu izgradnju pune putanje datoteke spajanjem točke montiranja i imena datoteke.
//       Uključuje osnovnu sanitizaciju kako bi se spriječili pokušaji pristupa direktorijima izvan točke montiranja
//       zamjenom sekvenci ".." sa "__".
//       POMAKNUTO OVDJE da bude definirana PRIJE handlera (download_handler, upload_post_handler) koji je koriste,
//       čime se izbjegava kompajlerska greška "implicit declaration".
// Argumenti:
//   - dst: Izlazni buffer za punu putanju.
//   - dst_size: Veličina izlaznog buffera 'dst'. Koristi se za sprječavanje preljeva.
//   - path1: Prvi dio putanje (obično točka montiranja, npr. "/sdcard").
//   - path2: Drugi dio putanje (obično ime datoteke).
static void build_filepath(char *dst, size_t dst_size, const char *path1, const char *path2)
{
    // Provjerava je li ime datoteke (path2) NULL.
    if (path2 == NULL)
    {
        ESP_LOGE(TAG_WEB, "build_filepath: path2 (ime datoteke) je NULL!");
        // Ako je, postavlja zadanu putanju s "unknown_filename" i osigurava null terminaciju.
        snprintf(dst, dst_size, "%s/unknown_filename", path1);
        dst[dst_size - 1] = '\0'; // Osiguraj null terminaciju
        return;
    }
    // Logira pokušaj izgradnje putanje (debug razina).
    ESP_LOGD(TAG_WEB, "Gradim punu putanju datoteke: path1='%s', path2='%s'", path1, path2);
    // Sastavlja punu putanju formatiranjem stringa. snprintf se koristi kako bi se spriječio prelijev buffera.
    snprintf(dst, dst_size, "%s/%s", path1, path2);
    dst[dst_size - 1] = '\0'; // Osiguraj null terminaciju, čak i ako je putanja skraćena.

    // Sanitizacija putanje: Traži i zamjenjuje sve pojave ".." sa "__".
    // Ovo je osnovna sigurnosna mjera protiv directory traversal napada,
    // gdje napadač pokušava pristupiti direktorijima izvan dozvoljenog opsega (npr. /sdcard/../etc/passwd).
    char *p;
    // Petlja traži substring ".." počevši od trenutne pozicije p (inicijalno dst).
    while ((p = strstr(dst, "..")) != NULL)
    {
        // Logira upozorenje kada se pronađe ".." i vrši zamjena.
        ESP_LOGW(TAG_WEB, "Sanitiziram '..' iz putanje: %s", dst);
        *p = '_';       // Zamijeni prvi '.' sa '_'
        *(p + 1) = '_'; // Zamijeni drugi '.' sa '_'
        // strstr u sljedećoj iteraciji nastavlja pretragu od mjesta nakon ove zamjene.
    }
    // Logira finalnu, sanitiziranu putanju (debug razina).
    ESP_LOGD(TAG_WEB, "Izgradjena putanja nakon sanitizacije: '%s'", dst);
}

// Funkcija: url_decode
// Opis: Dekodira URL-enkodirani string.
//       Npr. zamjenjuje %20 sa razmakom, %28 sa '(', %29 sa ')', + sa razmakom.
//       Koristi se za dekodiranje imena datoteka i drugih parametara dobivenih iz URL-a zahtjeva.
// Argumenti:
//   - src: Ulazni URL-enkodirani string.
//   - dst: Izlazni buffer za dekodirani string.
//   - dst_size: Veličina izlaznog buffera 'dst'.
// Povratna vrijednost: esp_err_t - ESP_OK ako je dekodiranje uspješno i stane u buffer, inače ESP_FAIL.
static esp_err_t url_decode(const char *src, char *dst, size_t dst_size)
{
    // Provjera validnosti ulaznih argumenata. Ako su null ili dst_size 0, vraća grešku.
    if (!src || !dst || dst_size == 0)
    {
        ESP_LOGE(TAG_WEB, "url_decode: Nevazeci ulazni parametri.");
        if (dst)
            dst[0] = '\0'; // Prazni izlazni buffer ako je validan, ali veličina 0.
        return ESP_FAIL;
    }

    char *d = dst;          // Pointer na trenutnu poziciju pisanja u izlazni buffer.
    const char *s = src;    // Pointer na trenutnu poziciju čitanja iz ulaznog stringa.
    size_t decoded_len = 0; // Brojač dekodiranih znakova.

    // Petlja prolazi kroz ulazni string znak po znak dok se ne dođe do kraja ulaza ('\0')
    // ili dok se ne napuni izlazni buffer (ostavi jedno mjesto za null-terminator).
    while (*s && decoded_len < dst_size - 1)
    {
        if (*s == '%') // Ako se naiđe na '%', to je početak URL-enkodirane sekvence (npr. %20).
        {
            // Provjerava postoje li sljedeća dva znaka (s+1 i s+2) i jesu li heksadecimalne znamenke.
            if (*(s + 1) && *(s + 2) && isxdigit((unsigned char)*(s + 1)) && isxdigit((unsigned char)*(s + 2)))
            {
                char hex[3] = {*(s + 1), *(s + 2), '\0'}; // Izdvoji heksadecimalni par (npr. "20").
                int ascii_val;
                sscanf(hex, "%x", &ascii_val); // Pretvara heksadecimalni string u integer (ASCII vrijednost).
                *d++ = (char)ascii_val;        // Dodaje dekodirani znak (npr. razmak za %20) u izlazni buffer.
                s += 3;                        // Pomakni ulazni pointer za 3 znaka (% + 2 hex znamenke).
                decoded_len++;                 // Povećaj brojač dekodiranih znakova.
            }
            else
            {
                // Ako % sekvenca nije ispravna (npr. % sam na kraju, %a, %% bez daljnjih znakova), logiraj grešku
                // i stavi '_' umjesto neispravne sekvence kao fallback.
                ESP_LOGE(TAG_WEB, "url_decode: Nevazeca ili nepotpuna %% sekvenca: %s", s);
                *d++ = '_';
                // Pokušaj pomaknuti ulazni pointer da preskoči barem '%'
                if (*(s + 1) && *(s + 2))
                    s += 3;
                else if (*(s + 1))
                    s += 2;
                else
                    s += 1;
                decoded_len++;
            }
        }
        else if (*s == '+') // Standardno URL dekodiranje također zamjenjuje '+' sa razmakom.
        {
            *d++ = ' ';    // Dodaje razmak u izlazni buffer.
            s++;           // Pomakni ulazni pointer za 1 znak.
            decoded_len++; // Povećaj brojač dekodiranih znakova.
        }
        else // Svi ostali znakovi se kopiraju direktno u izlazni buffer.
        {
            *d++ = *s;     // Kopira znak.
            s++;           // Pomakni ulazni pointer za 1 znak.
            decoded_len++; // Povećaj brojač dekodiranih znakova.
        }
    }

    *d = '\0'; // Null-terminira izlazni string.

    // Provjerava je li cijeli ulazni string obrađen. Ako *s nije '\0', znači da ulazni string
    // nije u potpunosti kopiran jer je izlazni buffer bio premali.
    if (*s != '\0')
    {
        // Ako nije, logiraj grešku - izlazni buffer je premali.
        ESP_LOGE(TAG_WEB, "url_decode: Izlazni buffer (%d) premali za ulazni string '%s'.", dst_size, src);
        if (dst_size > 0)
            dst[dst_size - 1] = '\0'; // Osigurava da je buffer null-terminiran čak i ako je skraćen.
        return ESP_FAIL;              // Vrati grešku.
    }

    // Logiraj uspješno dekodiranje (debug razina).
    ESP_LOGD(TAG_WEB, "url_decode: Dekodirano '%s' u '%s'", src, dst);
    return ESP_OK; // Vrati ESP_OK za uspjeh.
}

// Funkcija: httpd_send_template_chunk
// Opis: Pomoćna funkcija za slanje dijela ugrađenog HTML template-a kao chunk u HTTP odgovoru.
//       Koristi se za slanje dijelova HTML-a prije i poslije placeholder-a kada se
//       dinamički sadržaj umeće u template.
// Argumenti:
//   - req: Pointer na strukturu HTTP zahtjeva.
//   - chunk_start: Pointer na početak dijela HTML template-a za slanje.
//   - chunk_len: Duljina dijela template-a za slanje.
// Povratna vrijednost: esp_err_t - Vraća rezultat funkcije httpd_resp_send_chunk.
static esp_err_t httpd_send_template_chunk(httpd_req_t *req, const char *chunk_start, size_t chunk_len)
{
    // Provjeri je li duljina chunka veća od 0. Prazni chunkovi se ne šalju (osim završnog).
    if (chunk_len > 0)
    {
        // Šalje specificirani dio podataka kao jedan chunk HTTP odgovora.
        return httpd_resp_send_chunk(req, chunk_start, chunk_len);
    }
    // Ako je duljina 0, nema ništa za poslati (osim završnog chunk NULL, 0), vrati ESP_OK.
    return ESP_OK;
}

// Funkcija: send_message_response
// Opis: Generira i šalje HTML odgovor klijentu koristeći ugrađeni 'message.html' template.
//       Ovaj template se koristi za prikazivanje poruka (npr. uspjeh, greška) korisniku u browseru
//       na standardiziran način. Funkcija zamjenjuje placeholder-e (npr. %%MESSAGE_TITLE%%)
//       u template-u s dinamičkim sadržajem (naslov, klasa stila, tekst poruke).
// Argumenti:
//   - req: Pointer na strukturu HTTP zahtjeva.
//   - title: Naslov poruke koji se prikazuje u HTML stranici (<title> i unutar tijela).
//   - message_class: CSS klasa za element poruke (npr. "success", "error") za stiliziranje (boja, ikona).
//   - message_text: Stvarni tekst poruke koji se prikazuje korisniku.
// Povratna vrijednost: esp_err_t - ESP_OK ako je odgovor uspješno poslan, inače ESP_FAIL.
static esp_err_t send_message_response(httpd_req_t *req, const char *title, const char *message_class, const char *message_text)
{
    // Postavlja Content-Type zaglavlje odgovora na 'text/html' kako bi browser znao interpretirati sadržaj kao HTML.
    httpd_resp_set_type(req, "text/html");

    // Pointeri na početak i kraj ugrađenog message.html template-a u memoriji.
    const char *template_ptr = (const char *)message_html_start;
    const char *template_end_ptr = (const char *)message_html_end;
    esp_err_t ret = ESP_OK; // Varijabla za praćenje rezultata I/O operacija (slanja chunkova).

    // Niz placeholder stringova koji se traže u template-u i niz odgovarajućih vrijednosti kojima se zamjenjuju.
    const char *placeholders[] = {"%%MESSAGE_TITLE%%", "%%MESSAGE_CLASS%%", "%%MESSAGE_TEXT%%"};
    const char *values[] = {title, message_class, message_text};
    // Broj placeholder-a koje treba obraditi.
    int num_placeholders = sizeof(placeholders) / sizeof(placeholders[0]);

    // Petlja prolazi kroz svaki placeholder definiran u nizu 'placeholders'.
    for (int i = 0; i < num_placeholders; ++i)
    {
        // Traži trenutni placeholder string unutar ostatka template-a (počevši od template_ptr).
        const char *found_at = strstr(template_ptr, placeholders[i]);
        // Provjerava je li placeholder pronađen i je li unutar granica ugrađenog template-a.
        if (found_at && found_at < template_end_ptr)
        {
            // Ako je pronađen, šalje dio template-a PRIJE pronađenog placeholdera kao prvi chunk.
            // Duljina chunka je razlika pointera od template_ptr do found_at.
            if (httpd_send_template_chunk(req, template_ptr, found_at - template_ptr) != ESP_OK)
            {
                ESP_LOGE(TAG_WEB, "Greska pri slanju chunka prije placeholdera %s", placeholders[i]);
                return ESP_FAIL; // Ako slanje ne uspije, odmah vrati ESP_FAIL.
            }
            // Zatim šalje stvarnu vrijednost (content) za taj placeholder (iz niza 'values') kao sljedeći chunk.
            if (httpd_send_template_chunk(req, values[i], strlen(values[i])) != ESP_OK)
            {
                ESP_LOGE(TAG_WEB, "Greska pri slanju vrijednosti za placeholder %s", placeholders[i]);
                return ESP_FAIL; // Ako slanje ne uspije, odmah vrati ESP_FAIL.
            }
            // Pomakni pointer 'template_ptr' na poziciju IZA trenutnog placeholdera za nastavak pretraživanja
            // u sljedećoj iteraciji petlje.
            template_ptr = found_at + strlen(placeholders[i]);
        }
        else
        {
            // Ako placeholder nije pronađen u template-u, logiraj upozorenje.
            ESP_LOGW(TAG_WEB, "Placeholder %s nije pronadjen in message.html template-u", placeholders[i]);
            // U ovom slučaju, ostatak template-a će biti poslan bez zamjene ovog placeholdera.
        }
    }

    // Nakon obrade svih placeholdera, šalje se preostali dio template-a NAKON zadnjeg obrađenog placeholdera
    // (ili cijeli template ako nijedan placeholder nije pronađen ili obrađen).
    if (template_ptr < template_end_ptr)
    {
        if (httpd_send_template_chunk(req, template_ptr, template_end_ptr - template_ptr) != ESP_OK)
        {
            ESP_LOGE(TAG_WEB, "Greska pri slanju zavrsnog chunka message.html");
            return ESP_FAIL; // Ako slanje ne uspije, vrati ESP_FAIL.
        }
    }

    // Na kraju, šalje se prazan chunk (NULL pointer, 0 duljina) koji signalizira kraj HTTP odgovora
    // kada se koristi chunked encoding (što httpd_resp_send_chunk implicitno radi).
    if (httpd_resp_send_chunk(req, NULL, 0) != ESP_OK)
    {
        ESP_LOGE(TAG_WEB, "Greska pri slanju zavrsnog praznog chunka message.html");
        return ESP_FAIL; // Ako slanje ne uspije, vrati ESP_FAIL.
    }

    return ret; // Vraća finalni status (trebao bi biti ESP_OK ako prethodna slanja nisu vratila ESP_FAIL).
}

// --- HTTP Handler funkcije ---
// Svaka od ovih funkcija obrađuje specifični tip HTTP zahtjeva (metoda i URI).
// Funkcije primaju httpd_req_t *req strukturu koja sadrži sve detalje zahtjeva.

// Handler za GET zahtjeve na putanju /settings.
// Opis: Vraća trenutne postavke logiranja (konkretno log_on_boot status) u JSON formatu.
// Koristi funkciju settings_get_log_on_boot() iz settings.h.
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    // Postavlja Content-Type zaglavlje odgovora na 'application/json' jer se vraća JSON podatak.
    httpd_resp_set_type(req, "application/json");

    // Dohvaća trenutnu postavku 'log_on_boot' pozivanjem funkcije iz settings.h modula.
    bool log_on_boot = settings_get_log_on_boot();
    // Priprema JSON string odgovora. Formata je jednostavan: {"log_on_boot":true} ili {"log_on_boot":false}.
    char response[64]; // Buffer za JSON odgovor. Veličina 64 bi trebala biti dovoljna.
    snprintf(response, sizeof(response), "{\"log_on_boot\":%s}", log_on_boot ? "true" : "false");
    // Šalje generirani JSON string kao cijelo tijelo HTTP odgovora klijentu.
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN); // HTTPD_RESP_USE_STRLEN koristi strlen(response) za duljinu.
    // Vraća ESP_OK za uspješan završetak handlera.
    return ESP_OK;
}

/**
 * @brief Handler za POST /api/channel-configs (API) - sprema postavke.
 * @param req HTTP zahtjev koji u tijelu sadrži JSON podatke.
 * @return esp_err_t
 *
 * API endpoint koji prima nove konfiguracije kanala u JSON formatu od klijenta,
 * validira ih, i ako su ispravne, prosljeđuje ih 'settings' modulu za spremanje u NVS.
 */
static esp_err_t channel_configs_post_handler(httpd_req_t *req)
{
    char buf[1024]; // Buffer za primanje JSON podataka.
    int ret, remaining = req->content_len;

    // Provjera da li je sadržaj veći od našeg buffera.
    if (remaining >= sizeof(buf))
    {
        ESP_LOGE(TAG_WEB, "Veličina zahtjeva (%d) je prevelika za buffer (%d).", remaining, sizeof(buf));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Zahtjev prevelik");
    }

    // Primanje tijela HTTP zahtjeva.
    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0)
    {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
        {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0'; // Null-terminacija primljenog stringa.

    // Parsiranje JSON stringa.
    cJSON *root = cJSON_Parse(buf);
    // Validacija: Je li ispravan JSON, je li polje, ima li točno 8 elemenata?
    if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) != NUM_CHANNELS)
    {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON mora biti polje s 8 elemenata");
    }

    channel_config_t new_configs[NUM_CHANNELS];
    cJSON *elem = NULL;
    int i = 0;
    // Iteracija kroz elemente JSON polja.
    cJSON_ArrayForEach(elem, root)
    {
        cJSON *factor_item = cJSON_GetObjectItem(elem, "factor");
        cJSON *unit_item = cJSON_GetObjectItem(elem, "unit");

        // Validacija: Ima li svaki objekt 'factor' (broj) i 'unit' (string)?
        if (!cJSON_IsNumber(factor_item) || !cJSON_IsString(unit_item))
        {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Neispravan format elementa");
        }

        // Popunjavanje naše C strukture s podacima iz JSON-a.
        new_configs[i].scaling_factor = (float)factor_item->valuedouble; // Cast to float
        strncpy(new_configs[i].unit, unit_item->valuestring, MAX_UNIT_LEN - 1);
        new_configs[i].unit[MAX_UNIT_LEN - 1] = '\0'; // Osiguravanje null terminacije.
        i++;
    }
    cJSON_Delete(root); // Oslobađanje memorije od parsiranog JSON-a.

    // Prosljeđivanje novih konfiguracija 'settings' modulu za spremanje.
    if (settings_save_channel_configs(new_configs) == ESP_OK)
    {
        httpd_resp_sendstr(req, "Postavke uspješno spremljene.");
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Greška pri spremanju postavki.");
    }
    return ESP_OK;
}

// Handler za POST zahtjeve na putanju /settings.
// Opis: Prima JSON podatke u tijelu zahtjeva i ažurira postavke logiranja (konkretno log_on_boot).
// Očekuje JSON format: {"log_on_boot": true/false}.
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char buf[1024]; // Povećavamo buffer da stanu i konfiguracije kanala
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    // 1. Provjeri i spremi "log_on_boot" ako postoji
    cJSON *log_on_boot_item = cJSON_GetObjectItem(root, "log_on_boot");
    if (cJSON_IsBool(log_on_boot_item))
    {
        settings_set_log_on_boot(cJSON_IsTrue(log_on_boot_item));
    }

    // 2. Provjeri i spremi "channels" konfiguraciju ako postoji
    cJSON *channels_array = cJSON_GetObjectItem(root, "channels");
    if (cJSON_IsArray(channels_array) && cJSON_GetArraySize(channels_array) == NUM_CHANNELS)
    {
        channel_config_t new_configs[NUM_CHANNELS];
        cJSON *elem = NULL;
        int i = 0;
        cJSON_ArrayForEach(elem, channels_array)
        {
            cJSON *factor_item = cJSON_GetObjectItem(elem, "factor");
            cJSON *unit_item = cJSON_GetObjectItem(elem, "unit");
            if (cJSON_IsNumber(factor_item) && cJSON_IsString(unit_item))
            {
                new_configs[i].scaling_factor = factor_item->valuedouble;
                strncpy(new_configs[i].unit, unit_item->valuestring, MAX_UNIT_LEN - 1);
                new_configs[i].unit[MAX_UNIT_LEN - 1] = '\0';
                i++;
            }
        }
        // Spremi samo ako je parsirano točno 8 kanala
        if (i == NUM_CHANNELS)
        {
            settings_save_channel_configs(new_configs);
        }
    }

    cJSON_Delete(root);
    return httpd_resp_sendstr(req, "OK");
}

// Handler za GET zahtjeve na putanju /style.css.
// Opis: Poslužuje ugrađenu CSS datoteku (koja je ugrađena u firmware kao binarni podatak).
static esp_err_t css_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WEB, "Serviram ugradjeni /style.css"); // Logira informaciju o zahtjevu.
    // Postavlja Content-Type zaglavlje odgovora na 'text/css'. Ovo govori browseru da je sadržaj CSS.
    httpd_resp_set_type(req, "text/css");
    // Šalje cijeli sadržaj ugrađene CSS datoteke kao tijelo HTTP odgovora.
    // style_css_start je pointer na početak binarnog niza, a razlika style_css_end - style_css_start daje njegovu duljinu.
    httpd_resp_send(req, (const char *)style_css_start, style_css_end - style_css_start);
    // Vraća ESP_OK za uspješan završetak handlera.
    return ESP_OK;
}

// Handler za GET zahtjeve na putanju /script.js.
// Opis: Poslužuje ugrađenu JavaScript datoteku. Slično kao css_get_handler, ali za JS.
static esp_err_t js_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WEB, "Serviram ugradjeni /script.js"); // Logira informaciju.
    // Postavlja Content-Type zaglavlje odgovora na 'text/javascript'.
    httpd_resp_set_type(req, "text/javascript");
    // Šalje cijeli sadržaj ugrađene JavaScript datoteke.
    httpd_resp_send(req, (const char *)script_js_start, script_js_end - script_js_start);
    // Vraća ESP_OK.
    return ESP_OK;
}

// Handler za GET zahtjeve na root putanju /.
// Opis: Poslužuje ugrađenu glavnu HTML datoteku (index.html) - početna stranica web sučelja.
static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WEB, "Serviram ugradjeni /index.html"); // Logira informaciju.
    // Postavlja Content-Type zaglavlje odgovora na 'text/html'.
    httpd_resp_set_type(req, "text/html");
    // Šalje cijeli sadržaj ugrađene index.html datoteke.
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    // Vraća ESP_OK.
    return ESP_OK;
}




// Handler za GET zahtjeve na putanju /list.
// Opis: Generira HTML stranicu s popisom datoteka koje se nalaze na SD kartici i poslužuje je klijentu.
// Koristi ugrađeni list.html template i dinamički umeće popis datoteka.
static esp_err_t list_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WEB, "Serviram /list (iz template-a uz chunked slanje)"); // Logira informaciju.

    // Pokušava otvoriti direktorij koji je točka montiranja SD kartice (MOUNT_POINT, npr. "/sdcard").
    DIR *dir = opendir(MOUNT_POINT);
    // Provjerava je li otvaranje direktorija uspjelo.
    if (!dir)
    {
        // Ako ne uspije (npr. SD kartica nije inicijalizirana ili umetnuta), logira grešku
        // i šalje poruku o grešci klijentu koristeći pomoćnu funkciju send_message_response.
        ESP_LOGE(TAG_WEB, "Greska pri otvaranju direktorija %s (%s)", MOUNT_POINT, strerror(errno));
        return send_message_response(req, "Greska posluzitelja", "error", "Nije moguce otvoriti direktorij na SD kartici.");
    }
    ESP_LOGI(TAG_WEB, "Direktorij %s uspjesno otvoren.", MOUNT_POINT);

    // Priprema buffer za dinamičko generiranje HTML koda koji će predstavljati redove tablice s popisom datoteka.
    char *file_list_html = NULL;                    // Pointer na buffer.
    size_t file_list_len = 0;                       // Trenutna zauzeta duljina u bufferu.
    size_t file_list_buffer_size = 2048;            // Početna veličina buffera.
    file_list_html = malloc(file_list_buffer_size); // Alociraj početnu memoriju za buffer.
    // Provjera uspješnosti alokacije.
    if (!file_list_html)
    {
        // Ako alokacija ne uspije, logira grešku, zatvori otvoreni direktorij i pošalje grešku klijentu.
        ESP_LOGE(TAG_WEB, "Greska pri alokaciji pocetne memorije za popis datoteka!");
        closedir(dir);
        return send_message_response(req, "Greska posluzitelja", "error", "Interna greska (memorija).");
    }
    file_list_html[0] = '\0'; // Inicijaliziraj buffer kao prazan C string.

    struct dirent *entry; // Struktura za pohranu informacija o pojedinom unosu u direktoriju (ime, tip).
    ESP_LOGI(TAG_WEB, "Citaju se unosi u direktoriju %s...", MOUNT_POINT);
    // Petlja čita unose (datoteke i poddirektorije) iz direktorija jedan po jedan.
    // readdir vraća NULL kada više nema unosa.
    while ((entry = readdir(dir)) != NULL)
    {
        // Logira ime i tip pronađenog unosa (debug razina).
        ESP_LOGI(TAG_WEB, "readdir pronasao: ime='%s', tip=%d", entry->d_name, entry->d_type);
        // Provjerava je li pronađeni unos regularna datoteka (DT_REG). Zanemaruje direktorije (DT_DIR) i ostalo.
        if (entry->d_type == DT_REG)
        {
            ESP_LOGD(TAG_WEB, "Obrada regularne datoteke: %s", entry->d_name);
            // Procjena potrebne duljine buffera za HTML kod za *ovaj* red datoteke.
            // Uključuje prostor za ime filea (koje se može pojaviti više puta i biti URL enkodirano),
            // HTML tagove za red tablice (<tr>, <td>, <a>) i linkove.
            size_t js_escaped_len_estimate = strlen(entry->d_name) * 2 + 1;                             // Procjena za potencijalno JavaScript escapiranje imena u linkovima.
            size_t entry_html_len_estimate = js_escaped_len_estimate + strlen(entry->d_name) * 2 + 350; // Gruba procjena za cijeli HTML red s linkovima.

            // Provjerava ima li dovoljno mjesta u bufferu za dodavanje HTML koda za ovu datoteku.
            // Ako trenutna duljina + procijenjena duljina novog unosa premašuje veličinu buffera.
            if (file_list_len + entry_html_len_estimate >= file_list_buffer_size)
            {
                // Ako nema dovoljno mjesta, realocira buffer na veću veličinu.
                size_t new_size = file_list_buffer_size + entry_html_len_estimate + 1024; // Nova veličina: trenutna + procjena za novi unos + dodatnih 1KB kao padding.
                char *temp = realloc(file_list_html, new_size);                           // Pokušaj realokacije. realloc može vratiti NULL ako ne uspije.
                // Provjera uspješnosti realokacije.
                if (!temp)
                {
                    // Ako realokacija ne uspije, logiraj grešku, prekinuti čitanje direktorija
                    // i nastavi s postojećim (skraćenim) popisom.
                    ESP_LOGE(TAG_WEB, "Greska pri realokaciji memorije za popis datoteka, skracujem popis!");
                    break; // Izlaz iz while petlje (prekida dodavanje daljnjih datoteka).
                }
                file_list_html = temp;            // Ažuriraj pointer na novi, veći buffer.
                file_list_buffer_size = new_size; // Ažuriraj informaciju o novoj veličini buffera.
                ESP_LOGI(TAG_WEB, "Realociran buffer popisa datoteka na %d bajtova", new_size);
            }

            // Priprema buffere za URL-ove za preuzimanje i brisanje datoteke.
            char delete_url[512];   // Buffer za URL za brisanje.
            char download_url[512]; // Buffer za URL za preuzimanje.

            // URL-enkodiranje imena datoteke kako bi se sigurno koristilo u URL-u kao query parametar.
            // Npr. razmaci postaju %20, zagrade %28/%29.
            // Ovo je ručno implementirano, osnovno URL enkodiranje. Za potpunu usklađenost,
            // koristile bi se standardne funkcije URL enkodiranja ako su dostupne u ESP-IDF.
            char url_encoded_filename[sizeof(entry->d_name) * 3 + 1]; // Buffer dovoljno velik za najgori slučaj enkodiranja.
            char *p_in_url = entry->d_name;                           // Pointer za čitanje iz izvornog imena filea.
            char *p_out_url = url_encoded_filename;                   // Pointer za pisanje u izlazni buffer.
            // Petlja prolazi kroz ime datoteke i enkodira specifične znakove.
            while (*p_in_url && (p_out_url - url_encoded_filename) < (sizeof(url_encoded_filename) - 4))
            {
                if (*p_in_url == ' ')
                {
                    *p_out_url++ = '%';
                    *p_out_url++ = '2';
                    *p_out_url++ = '0';
                }
                else if (*p_in_url == '(')
                {
                    *p_out_url++ = '%';
                    *p_out_url++ = '2';
                    *p_out_url++ = '8';
                }
                else if (*p_in_url == ')')
                {
                    *p_out_url++ = '%';
                    *p_out_url++ = '2';
                    *p_out_url++ = '9';
                }
                else if (*p_in_url == '&')
                {
                    *p_out_url++ = '%';
                    *p_out_url++ = '2';
                    *p_out_url++ = '6';
                } // Dodaj enkodiranje za &
                else if (*p_in_url == '=')
                {
                    *p_out_url++ = '%';
                    *p_out_url++ = '3';
                    *p_out_url++ = 'D';
                } // Dodaj enkodiranje za =
                else if (*p_in_url == '?')
                {
                    *p_out_url++ = '%';
                    *p_out_url++ = '3';
                    *p_out_url++ = 'F';
                } // Dodaj enkodiranje za ?
                else if (*p_in_url == '/')
                {
                    *p_out_url++ = '%';
                    *p_out_url++ = '2';
                    *p_out_url++ = 'F';
                } // Dodaj enkodiranje za /
                else
                {
                    *p_out_url++ = *p_in_url;
                } // Ostale znakove kopiraj direktno.
                p_in_url++;
            }
            *p_out_url = '\0'; // Null-terminiraj enkodirani string.
            // Provjera je li enkodirani string skraćen zbog veličine buffera.
            if ((p_out_url - url_encoded_filename) >= sizeof(url_encoded_filename))
            {
                url_encoded_filename[sizeof(url_encoded_filename) - 1] = '\0';          // Osiguraj null-terminaciju na kraju buffera.
                ESP_LOGW(TAG_WEB, "URL encoded filename truncated: %s", entry->d_name); // Logiraj upozorenje.
            }

            // Sastavljanje kompletnih URL-ova za preuzimanje i brisanje.
            // Koristi snprintf za formatiranje, uključujući enkodirano ime datoteke kao query parametar 'file'.
            // Provjera prelijevanja snprintf-a.
            if (snprintf(download_url, sizeof(download_url), "/download?file=%s", url_encoded_filename) >= sizeof(download_url))
            {
                ESP_LOGE(TAG_WEB, "Download URL truncation error for %s", entry->d_name);
                download_url[sizeof(download_url) - 1] = '\0'; // Osiguraj null-terminaciju.
            }
            if (snprintf(delete_url, sizeof(delete_url), "/delete?file=%s", url_encoded_filename) >= sizeof(delete_url))
            {
                ESP_LOGE(TAG_WEB, "Delete URL truncation error for %s", entry->d_name);
                delete_url[sizeof(delete_url) - 1] = '\0'; // Osiguraj null-terminaciju.
            }

            // Generiranje HTML koda za jedan red tablice (<tr>...</tr>) koji prikazuje ime datoteke
            // i sadrži linkove za preuzimanje i brisanje.
            // Koristi snprintf za formatiranje, dodajući generirani HTML na kraj postojećeg file_list_html buffera.
            int written_len = snprintf(file_list_html + file_list_len, file_list_buffer_size - file_list_len,
                                       "<tr><td>%s</td><td><a href=\"%s\">Preuzmi</a></td><td><a href=\"%s\" class=\"delete-link\">Obriši</a></td></tr>",
                                       entry->d_name, download_url, delete_url);
            // Provjera je li snprintf bio uspješan (vratio > 0) i je li stao u preostali prostor buffera.
            if (written_len > 0 && (size_t)written_len < (file_list_buffer_size - file_list_len))
            {
                file_list_len += written_len; // Ažuriraj trenutnu zauzetu duljinu buffera.
            }
            else
            {
                // Ako snprintf ne uspije ili je buffer premali, logiraj grešku.
                ESP_LOGE(TAG_WEB, "snprintf greska pri pisanju unosa datoteke ili buffer premali za: %s", entry->d_name);
                if (written_len > 0)
                    file_list_len = file_list_buffer_size - 1; // U slučaju preljeva, postavi duljinu na maksimalnu kako bi se izbjeglo pisanje izvan granica.
                // NAPOMENA: Ovo skraćivanje može rezultirati nekompletnim HTML-om ako se dogodi unutar taga.
            }
        }
    }
    closedir(dir); // Zatvori direktorij nakon čitanja unosa.
    ESP_LOGI(TAG_WEB, "Zavrseno citanje unosa u direktoriju. Ukupna duljina HTML liste: %d", file_list_len);

    // Postavlja Content-Type odgovora na 'text/html' jer se poslužuje HTML stranica.
    httpd_resp_set_type(req, "text/html");

    // Pointeri na početak i kraj ugrađenog list.html template-a.
    const char *template_start_ptr = (const char *)list_html_start;
    const char *template_end_ptr = (const char *)list_html_end;
    const char *placeholder_str = "%%FILE_LIST_ROWS%%"; // Placeholder string u template-u gdje treba umetnuti popis datoteka.
    // Traži poziciju placeholder-a u template-u.
    const char *placeholder_found_at = strstr(template_start_ptr, placeholder_str);

    // Provjerava je li placeholder pronađen u template-u i je li unutar granica.
    if (placeholder_found_at && placeholder_found_at < template_end_ptr)
    {
        ESP_LOGI(TAG_WEB, "Placeholder '%s' pronadjen. Saljem dijelove template-a.", placeholder_str);
        // Šalje dio template-a PRIJE placeholdera kao prvi chunk.
        // Razlika pointera daje duljinu chunka.
        if (httpd_send_template_chunk(req, template_start_ptr, placeholder_found_at - template_start_ptr) != ESP_OK)
            goto fail_list_handler;
        // Zatim šalje dinamički generirani HTML popis datoteka kao sljedeći chunk.
        if (httpd_send_template_chunk(req, file_list_html, file_list_len) != ESP_OK)
            goto fail_list_handler;
        // Pomakni pointer 'template_start_ptr' na poziciju IZA placeholdera za slanje preostalog dijela template-a.
        template_start_ptr = placeholder_found_at + strlen(placeholder_str);
    }
    else
    {
        // Ako placeholder nije pronađen (greška u template file-u), logiraj grešku i nastavi slati samo ostatak template-a.
        ESP_LOGE(TAG_WEB, "Placeholder '%s' nije pronadjen u list.html! Saljem template bez popisa.", placeholder_str);
    }

    // Šalje preostali dio template-a NAKON placeholdera (ili cijeli template ako placeholder nije pronađen).
    if (template_start_ptr < template_end_ptr)
    {
        if (httpd_send_template_chunk(req, template_start_ptr, template_end_ptr - template_start_ptr) != ESP_OK)
            goto fail_list_handler;
    }

    // Na kraju, šalje se prazan chunk (NULL, 0) koji signalizira kraj HTTP odgovora.
    if (httpd_resp_send_chunk(req, NULL, 0) != ESP_OK)
        goto fail_list_handler;

    ESP_LOGI(TAG_WEB, "/list handler zavrsio uspjesno.");
    free(file_list_html); // Oslobađa memoriju zauzetu za dinamički generirani HTML popis.
    return ESP_OK;        // Vraća ESP_OK za uspjeh.

// Labela za skok u slučaju greške pri slanju chunkova.
fail_list_handler:
    ESP_LOGE(TAG_WEB, "/list handler neuspjesan.");
    if (file_list_html)
        free(file_list_html); // Ako je memorija alocirana, oslobodi je.
    // HTTP server će automatski prekinuti vezu ako handler vrati ESP_FAIL.
    return ESP_FAIL; // Vraća ESP_FAIL.
}

// Handler za GET zahtjeve na putanju /download.
// Opis: Omogućava preuzimanje datoteka s SD kartice klijentu.
// Ime datoteke za preuzimanje se prosljeđuje kao query parametar u URL-u (npr. /download?file=ime_datoteke.txt).
static esp_err_t download_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WEB, "Serviram /download"); // Logira informaciju o zahtjevu.
    char filepath[FILE_PATH_MAX];            // Buffer za punu putanju do datoteke na SD kartici.
    char *query_buf = NULL;                  // Buffer za čitanje query stringa iz URL-a.
    size_t query_buf_len;                    // Duljina query stringa.
    char filename_query[128 + 1];            // Buffer za vrijednost 'file' query parametra (ime datoteke kao string).
    esp_err_t send_ret = ESP_OK;             // Varijabla za praćenje statusa operacije slanja podataka filea.

    // Dohvaća duljinu query stringa iz URL-a zahtjeva. Dodaje 1 za null-terminaciju.
    query_buf_len = httpd_req_get_url_query_len(req) + 1;
    // Provjerava postoji li query string (duljina > 1 znači da postoji bar jedan znak osim null-terminatora).
    if (query_buf_len <= 1)
    {
        // Ako query string ne postoji (npr. URL je samo /download bez ?file=...), šalje poruku o grešci klijentu.
        return send_message_response(req, "Greska preuzimanja", "error", "Nedostaje parametar datoteke za preuzimanje.");
    }
    // Alocira memoriju za query string buffer.
    query_buf = malloc(query_buf_len);
    // Provjerava uspješnost alokacije.
    if (!query_buf)
    {
        // Ako alokacija ne uspije, šalje poruku o grešci (interna greška servera).
        return send_message_response(req, "Greska preuzimanja", "error", "Interna greska servera (memorija).");
    }

    // Dohvaća cijeli query string iz URL-a u alocirani buffer.
    // Zatim pokušava izdvojiti vrijednost specifičnog parametra "file" iz query stringa u filename_query buffer.
    if (httpd_req_get_url_query_str(req, query_buf, query_buf_len) != ESP_OK ||
        httpd_query_key_value(query_buf, "file", filename_query, sizeof(filename_query)) != ESP_OK)
    {
        // Ako dohvat query stringa ili izdvajanje parametra "file" ne uspije, šalje grešku.
        free(query_buf);
        query_buf = NULL; // Oslobađa alociranu memoriju.
        return send_message_response(req, "Greška preuzimanja", "error", "Nevažeći parametar datoteke.");
    }
    free(query_buf);
    query_buf = NULL; // Oslobađa memoriju za query string jer više nije potreban.

    char decoded_filename[sizeof(filename_query)]; // Buffer za dekodirano ime datoteke.
    // Dekodira URL-enkodirano ime datoteke dobiveno iz query parametra.
    if (url_decode(filename_query, decoded_filename, sizeof(decoded_filename)) != ESP_OK)
    {
        // Ako dekodiranje ne uspije, logira grešku i šalje poruku o grešci klijentu.
        ESP_LOGE(TAG_WEB, "Download error: Greska pri URL dekodiranju imena datoteke '%s'", filename_query);
        return send_message_response(req, "Greska preuzimanja", "error", "Greska pri dekodiranju imena datoteke za preuzimanje.");
    }

    // Sastavlja punu putanju do datoteke na SD kartici koristeći točku montiranja i dekodirano ime datoteke.
    // Koristi build_filepath za sigurnu konstrukciju putanje i sanitizaciju.
    build_filepath(filepath, sizeof(filepath), MOUNT_POINT, decoded_filename);
    ESP_LOGI(TAG_WEB, "Pokusaj preuzimanja datoteke: %s (dekodirano: '%s')", filepath, decoded_filename);

    // Pokušava otvoriti datoteku na SD kartici za čitanje u binarnom modu ("rb").
    FILE *file = fopen(filepath, "rb");
    // Provjerava je li otvaranje datoteke uspjelo.
    if (!file)
    {
        // Ako ne uspije (npr. datoteka ne postoji, greška na SD kartici), logira grešku (uključujući sistemsku grešku iz errno)
        // i šalje poruku o grešci klijentu koristeći send_message_response.
        ESP_LOGE(TAG_WEB, "Greska pri otvaranju datoteke za citanje: %s (%s)", filepath, strerror(errno));
        char error_msg[sizeof(decoded_filename) + 100]; // Buffer za poruku greške.
        snprintf(error_msg, sizeof(error_msg), "Datoteka '%s' nije pronadjena ili se ne moze otvoriti.", decoded_filename);
        return send_message_response(req, "Greska preuzimanja", "error", error_msg);
    }

    // Postavlja Content-Disposition zaglavlje u HTTP odgovoru.
    // Ovo zaglavlje govori browseru kako da tretira primljeni sadržaj.
    // "attachment; filename=\"%s\"" sugerira browseru da ponudi datoteku na preuzimanje
    // s navedenim imenom (dekodirano ime datoteke).
    char content_disposition[sizeof(decoded_filename) + 50]; // Buffer za Content-Disposition zaglavlje.
    snprintf(content_disposition, sizeof(content_disposition), "attachment; filename=\"%s\"", decoded_filename);
    httpd_resp_set_hdr(req, "Content-Disposition", content_disposition);

    // Čita i šalje sadržaj datoteke u manjim dijelovima (chunkovima).
    // Ovo je efikasnije od učitavanja cijelog file-a u memoriju odjednom, pogotovo za velike file-ove.
    char file_buf[1024]; // Buffer za čitanje dijelova datoteke iz file-a.
    size_t read_bytes;   // Broj bajtova pročitanih u jednom čitanju.
    // Petlja se izvodi dok se iz file-a čita bar 1 bajt.
    do
    {
        // Čita dio datoteke (do veličine file_buf) u buffer.
        read_bytes = fread(file_buf, 1, sizeof(file_buf), file);
        // Provjerava je li išta pročitano.
        if (read_bytes > 0)
        {
            // Ako da, šalje pročitani dio kao chunk u HTTP odgovoru klijentu.
            if (httpd_resp_send_chunk(req, file_buf, read_bytes) != ESP_OK)
            {
                // Ako slanje chunka ne uspije (npr. klijent prekine vezu), logira grešku
                // i postavlja status greške.
                ESP_LOGE(TAG_WEB, "Greska pri slanju chunka datoteke %s", filepath);
                send_ret = ESP_FAIL; // Označava da je došlo do greške pri slanju.
                break;               // Prekida petlju čitanja i slanja.
            }
        }
    } while (read_bytes > 0); // Ponavljaj dok se iz file-a čita bar 1 bajt.

    // Nakon završetka čitanja i slanja svih chunkova, ako nije bilo grešaka pri slanju (send_ret je i dalje ESP_OK),
    // pošalji završni prazni chunk (NULL, 0). Ovo signalizira kraj tijela HTTP odgovora.
    if (send_ret == ESP_OK)
    {
        if (httpd_resp_send_chunk(req, NULL, 0) != ESP_OK)
        {
            // Ako slanje završnog chunka ne uspije, logira grešku i postavlja status greške.
            ESP_LOGE(TAG_WEB, "Greska pri slanju zavrsnog praznog chunka datoteke %s", filepath);
            send_ret = ESP_FAIL;
        }
    }

    fclose(file); // Zatvori datoteku nakon što je pročitana (ili ako je došlo do greške).
    // Logira uspješan završetak preuzimanja ako nije bilo grešaka pri slanju.
    if (send_ret == ESP_OK)
        ESP_LOGI(TAG_WEB, "Preuzimanje datoteke zavrseno: %s", decoded_filename);
    return send_ret; // Vraća finalni status operacije slanja.
}

// Handler za GET zahtjeve na putanju /delete.
// Opis: Omogućava brisanje datoteka s SD kartice.
// Ime datoteke za brisanje se prosljeđuje kao query parametar u URL-u (npr. /delete?file=ime_datoteke.txt).
// Vraća JSON odgovor koji indicira status operacije (uspjeh/greška).
static esp_err_t delete_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WEB, "Serviram /delete (AJAX zahtjev)"); // Logira informaciju.
    char filepath[FILE_PATH_MAX];                         // Buffer za punu putanju do datoteke.
    char *query_buf = NULL;                               // Buffer za query string.
    size_t query_buf_len;                                 // Duljina query stringa.
    char filename_query[128 + 1];                         // Buffer za vrijednost 'file' query parametra (ime datoteke).
    esp_err_t op_status = ESP_FAIL;                       // Varijabla za praćenje statusa operacije brisanja. Inicijalizirana na grešku.

    // Postavlja Content-Type zaglavlje odgovora na 'application/json' jer će se vratiti JSON odgovor.
    httpd_resp_set_type(req, "application/json");

    // Kreira korijenski JSON objekt koji će sadržavati status i poruku odgovora.
    cJSON *root_json = cJSON_CreateObject();
    // Provjerava uspješnost kreiranja JSON objekta.
    if (root_json == NULL)
    {
        ESP_LOGE(TAG_WEB, "Greska pri kreiranju cJSON root objekta za delete odgovor");
        // Ako kreiranje JSON-a ne uspije, šalje internu grešku servera.
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON creation failed");
        return ESP_FAIL; // Vraća ESP_FAIL.
    }

    // Dohvaća duljinu query stringa.
    query_buf_len = httpd_req_get_url_query_len(req) + 1; // +1 za null-terminaciju.
    // Provjerava postoji li query string.
    if (query_buf_len <= 1)
    {
        // Ako ne postoji, logira grešku i dodaje detalje o grešci (status "error", poruka) u JSON odgovor.
        ESP_LOGE(TAG_WEB, "Greska pri brisanju: Nedostaje parametar datoteke u query.");
        cJSON_AddStringToObject(root_json, "status", "error");
        cJSON_AddStringToObject(root_json, "message", "Nedostaje parametar datoteke za brisanje.");
        goto send_json_delete_response; // Skok na labelu za slanje JSON odgovora.
    }

    // Alocira memoriju za query string buffer.
    query_buf = malloc(query_buf_len);
    // Provjerava uspješnost alokacije.
    if (!query_buf)
    {
        // Ako alokacija ne uspije, logira grešku i dodaje grešku u JSON odgovor.
        ESP_LOGE(TAG_WEB, "Greska pri brisanju: Malloc neuspjesan za query_buf.");
        cJSON_AddStringToObject(root_json, "status", "error");
        cJSON_AddStringToObject(root_json, "message", "Interna greska servera (memorija).");
        goto send_json_delete_response; // Skok na slanje JSON odgovora.
    }

    // Dohvaća query string i pokušava izdvojiti vrijednost parametra "file" u filename_query buffer.
    if (httpd_req_get_url_query_str(req, query_buf, query_buf_len) != ESP_OK ||
        httpd_query_key_value(query_buf, "file", filename_query, sizeof(filename_query)) != ESP_OK)
    {
        // Ako dohvat query stringa ili izdvajanje parametra "file" ne uspije, logira grešku i dodaje grešku u JSON odgovor.
        ESP_LOGE(TAG_WEB, "Greska pri brisanju: Nevazeci parametar datoteke u query.");
        cJSON_AddStringToObject(root_json, "status", "error");
        cJSON_AddStringToObject(root_json, "message", "Nevazeci parametar datoteke za brisanje.");
        goto send_json_delete_response; // Skok na slanje JSON odgovora.
    }

    char decoded_filename[sizeof(filename_query)]; // Buffer za dekodirano ime datoteke.
    // Dekodira URL-enkodirano ime datoteke dobiveno iz query parametra.
    if (url_decode(filename_query, decoded_filename, sizeof(decoded_filename)) != ESP_OK)
    {
        // Ako dekodiranje ne uspije, logira grešku i dodaje grešku u JSON odgovor.
        ESP_LOGE(TAG_WEB, "Delete error: Greska pri URL dekodiranju imena datoteke '%s'", filename_query);
        cJSON_AddStringToObject(root_json, "status", "error");
        cJSON_AddStringToObject(root_json, "message", "Greska pri dekodiranju imena datoteke za brisanje.");
        goto send_json_delete_response; // Skok na slanje JSON odgovora.
    }

    // Sastavlja punu putanju do datoteke na SD kartici koristeći točku montiranja i dekodirano ime datoteke.
    // Koristi build_filepath za sigurnu konstrukciju putanje i sanitizaciju ".." sekvenci.
    build_filepath(filepath, sizeof(filepath), MOUNT_POINT, decoded_filename);
    ESP_LOGI(TAG_WEB, "Pokusaj brisanja datoteke: '%s' (dekodirano: '%s')", filepath, decoded_filename);

    // Pokušava obrisati datoteku na navedenoj putanji koristeći POSIX funkciju unlink().
    if (unlink(filepath) == 0)
    {
        // Ako brisanje uspije, logira uspjeh i dodaje uspješan status ("success") i poruku u JSON odgovor.
        ESP_LOGI(TAG_WEB, "Datoteka uspjesno obrisana: '%s'", decoded_filename);
        char success_msg[sizeof(decoded_filename) + 100]; // Buffer za poruku o uspjehu.
        snprintf(success_msg, sizeof(success_msg), "Datoteka '%s' je uspjesno obrisana.", decoded_filename);

        cJSON_AddStringToObject(root_json, "status", "success");
        cJSON_AddStringToObject(root_json, "message", success_msg);
        op_status = ESP_OK; // Postavlja status operacije na uspjeh.
    }
    else
    {
        // Ako brisanje ne uspije, logira grešku (uključujući sistemsku grešku iz errno)
        // i dodaje grešku ("error") i detaljnu poruku u JSON odgovor.
        ESP_LOGE(TAG_WEB, "Greska pri brisanju datoteke: '%s' (%s)", filepath, strerror(errno));
        char error_msg[sizeof(decoded_filename) + 128]; // Buffer za poruku greške.
        snprintf(error_msg, sizeof(error_msg), "Nije moguce obrisati datoteku '%s'. Greška: %s", decoded_filename, strerror(errno));
        cJSON_AddStringToObject(root_json, "status", "error");
        cJSON_AddStringToObject(root_json, "message", error_msg);
    }

// Labela za skok na kraj funkcije, gdje se šalje generirani JSON odgovor.
send_json_delete_response:
    if (query_buf)
        free(query_buf); // Oslobodi memoriju alociranu za query string ako je bila alocirana.

    // Pretvara cJSON objekt (koji sadrži status i poruku) u formatirani JSON string.
    char *json_string = cJSON_PrintUnformatted(root_json);
    // Provjerava uspješnost pretvorbe JSON objekta u string.
    if (json_string == NULL)
    {
        ESP_LOGE(TAG_WEB, "Greska pri pretvaranju cJSON objekta u string za delete odgovor.");
        if (root_json)
            cJSON_Delete(root_json); // Pokušaj osloboditi root_json ako postoji
        // Šalje internu grešku servera ako JSON string ne može biti generiran.
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON print failed");
    }
    else
    {
        // Ako je pretvorba uspješna, šalje JSON string kao tijelo HTTP odgovora klijentu.
        httpd_resp_send(req, json_string, strlen(json_string));
        ESP_LOGD(TAG_WEB, "Poslan JSON delete odgovor: %s", json_string); // Logira poslani JSON (debug razina).
        cJSON_free(json_string);                                          // Oslobađa memoriju zauzetu za JSON string (koju je alocirao cJSON_PrintUnformatted).
    }
    cJSON_Delete(root_json); // Oslobađa memoriju zauzetu za cJSON objekt.

    return op_status; // Vraća status operacije brisanja (ESP_OK za uspjeh, ESP_FAIL za grešku).
}

/**
 * @brief Handler for GET requests to the `/delete_all` URI.
 * Deletes all regular files from the SD card's mount point.
 * Returns a JSON response indicating the operation status (success/error).
 * @param req Pointer to the HTTP request structure.
 * @return esp_err_t ESP_OK on success.
 */
static esp_err_t delete_all_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WEB, "Serving /delete_all (AJAX request)");
    
    // Set response type to JSON
    httpd_resp_set_type(req, "application/json");

    cJSON *root_json = cJSON_CreateObject();
    if (root_json == NULL) {
        ESP_LOGE(TAG_WEB, "Error creating cJSON root object for delete_all response");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON creation failed");
        return ESP_FAIL;
    }

    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG_WEB, "Error opening directory %s (%s)", MOUNT_POINT, strerror(errno));
        cJSON_AddStringToObject(root_json, "status", "error");
        cJSON_AddStringToObject(root_json, "message", "Could not open SD card directory.");
        goto send_json_delete_all_response;
    }

    struct dirent *entry;
    char filepath[FILE_PATH_MAX];
    int deleted_count = 0;
    int failed_count = 0;

    while ((entry = readdir(dir)) != NULL) {
        // Skip directories and special entries like "." and ".."
        if (entry->d_type == DT_REG) { // Only process regular files
            // Skip system volume information directory if present
            if (strcmp(entry->d_name, "System Volume Information") == 0) {
                ESP_LOGI(TAG_WEB, "Skipping system directory: %s", entry->d_name);
                continue;
            }

            snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, entry->d_name);
            filepath[sizeof(filepath) - 1] = '\0'; // Ensure null termination

            if (unlink(filepath) == 0) {
                ESP_LOGI(TAG_WEB, "Deleted file: %s", filepath);
                deleted_count++;
            } else {
                ESP_LOGE(TAG_WEB, "Failed to delete file: %s (%s)", filepath, strerror(errno));
                failed_count++;
            }
        }
    }
    closedir(dir);

    if (deleted_count > 0 || failed_count > 0) {
        char msg[128];
        if (failed_count == 0) {
            snprintf(msg, sizeof(msg), "Obrisano %d datoteka.", deleted_count);
            cJSON_AddStringToObject(root_json, "status", "success");
        } else if (deleted_count == 0) {
            snprintf(msg, sizeof(msg), "Greška: Nije moguće obrisati datoteke. %d neuspjelo.", failed_count);
            cJSON_AddStringToObject(root_json, "status", "error");
        } else {
            snprintf(msg, sizeof(msg), "Obrisano %d datoteka, %d neuspjelo.", deleted_count, failed_count);
            cJSON_AddStringToObject(root_json, "status", "warning"); // Custom status for partial success
        }
        cJSON_AddStringToObject(root_json, "message", msg);
    } else {
        cJSON_AddStringToObject(root_json, "status", "info");
        cJSON_AddStringToObject(root_json, "message", "Nema datoteka za brisanje.");
    }

send_json_delete_all_response:
    char *json_string = cJSON_PrintUnformatted(root_json);
    if (json_string == NULL) {
        ESP_LOGE(TAG_WEB, "Error converting cJSON object to string for delete_all response.");
        if (root_json) cJSON_Delete(root_json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON print failed");
        return ESP_FAIL;
    } else {
        httpd_resp_send(req, json_string, strlen(json_string));
        cJSON_free(json_string);
    }
    cJSON_Delete(root_json);
    return ESP_OK;
}


// Handler za POST zahtjeve na putanju /upload.
// Opis: Omogućava upload datoteka s klijenta na SD karticu na ESP32.
// Podaci se očekuju u multipart/form-data formatu, što je uobičajeno za slanje fileova putem web formi.
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WEB, "/upload handler pozvan."); // Logira početak handlera.

    char filepath[FILE_PATH_MAX]; // Buffer za punu putanju do datoteke na SD kartici gdje će se file spremiti.
    char *buf = NULL;             // Buffer za primanje dijelova (chunkova) uploadanih podataka iz tijela HTTP zahtjeva.
    char *filename = NULL;        // Varijabla za spremanje imena datoteke izdvojenog iz HTTP zaglavlja (Content-Disposition).
    FILE *fd = NULL;              // File deskriptor (pointer na FILE strukturu) za pisanje u datoteku na SD kartici.

    int ret;                          // Varijabla za spremanje povratnih vrijednosti funkcija (npr. httpd_req_recv, fwrite).
    int remaining = req->content_len; // Broj bajtova koji preostaju za primanje u tijelu HTTP zahtjeva.
                                      // req->content_len sadrži ukupnu duljinu tijela zahtjeva.

    ESP_LOGI(TAG_WEB, "Zahtjev za upload. Content-Length: %d bajtova", remaining); // Logira očekivanu veličinu uploada.

    // Postavlja Content-Type zaglavlje odgovora na 'application/json' jer će server vratiti JSON status operacije.
    httpd_resp_set_type(req, "application/json");

    cJSON *root_json = NULL;  // Korijenski JSON objekt za odgovor (inicijalno NULL).
    char *json_string = NULL; // Buffer za konačni JSON string odgovora (inicijalno NULL).

    // Provjerava Content-Length zahtjeva. Ako je 0 ili manji, nema podataka za upload ili je zaglavlje neispravno.
    if (remaining <= 0)
    {
        ESP_LOGE(TAG_WEB, "Content-Length je %d. Nema podataka za upload ili nevazeca duljina.", remaining);
        root_json = cJSON_CreateObject(); // Kreira JSON objekt za signaliziranje greške.
        cJSON_AddStringToObject(root_json, "status", "error");
        cJSON_AddStringToObject(root_json, "message", "Nema podataka za upload (content length je 0 ili neispravan).");
        goto send_json_upload_response; // Skok na dio za slanje JSON odgovora.
    }

    // Alocira memoriju za privremeni buffer koji će primati chunkove podataka.
    buf = malloc(UPLOAD_BUFFER_SIZE);
    // Provjerava uspješnost alokacije.
    if (!buf)
    {
        ESP_LOGE(TAG_WEB, "Greska pri alokaciji memorije za upload buffer (velicina: %d)", UPLOAD_BUFFER_SIZE);
        root_json = cJSON_CreateObject(); // Kreira JSON objekt za grešku nedostatka memorije.
        cJSON_AddStringToObject(root_json, "status", "error");
        cJSON_AddStringToObject(root_json, "message", "Interna greska servera (memorija za buffer).");
        // Pokušava poslati ovu grešku kao JSON, ako ne uspije, šalje generičku HTTP 500 grešku.
        json_string = cJSON_PrintUnformatted(root_json);
        if (json_string)
        {
            httpd_resp_send(req, json_string, strlen(json_string));
            cJSON_free(json_string);
        }
        else
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory and create JSON");
        }
        cJSON_Delete(root_json); // Oslobađa memoriju cJSON objekta.
        return ESP_FAIL;         // Vraća ESP_FAIL.
    }
    ESP_LOGI(TAG_WEB, "Upload buffer alociran (velicina: %d).", UPLOAD_BUFFER_SIZE);

    // Zastavice i bufferi potrebni za parsiranje multipart/form-data tijela zahtjeva.
    // Multipart poruka se sastoji od dijelova odvojenih "boundary" stringom.
    // Prvi dio obično sadrži zaglavlja (kao Content-Disposition s imenom filea),
    // nakon čega slijedi stvarni binarni sadržaj datoteke.
    bool file_data_started = false;   // Zastavica koja postaje true kada počnu stvarni podaci datoteke.
    char boundary[128] = {0};         // String koji predstavlja graničnik između dijelova multipart poruke. Izdvaja se iz Content-Type zaglavlja.
    char content_type_hdr[200] = {0}; // Buffer za Content-Type zaglavlje zahtjeva.

    ESP_LOGI(TAG_WEB, "Dohvacam Content-Type zaglavlje."); // Logira pokušaj dohvaćanja Content-Type-a.
    // Dohvaća Content-Type zaglavlje iz zahtjeva u buffer content_type_hdr.
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type_hdr, sizeof(content_type_hdr)) == ESP_OK)
    {
        ESP_LOGI(TAG_WEB, "Content-Type: %s", content_type_hdr); // Logira vrijednost zaglavlja.
        // Traži string "boundary=" unutar Content-Type zaglavlja.
        char *boundary_ptr = strstr(content_type_hdr, "boundary=");
        if (boundary_ptr)
        {
            boundary_ptr += strlen("boundary="); // Pomakni pointer na početak stvarne vrijednosti boundary stringa.
                                                 // Kopiraj vrijednost boundary i dodaj "--" na početak (multipart boundary stringovi počinju s "--").
                                                 // Koristi snprintf za sigurno kopiranje, provjeravajući prelijevanje buffera.
            if (snprintf(boundary, sizeof(boundary), "--%s", boundary_ptr) >= sizeof(boundary))
            {
                ESP_LOGE(TAG_WEB, "Boundary string predugacak."); // Logira grešku ako je boundary predugačak.
                root_json = cJSON_CreateObject();                 // Pripremi JSON grešku.
                cJSON_AddStringToObject(root_json, "status", "error");
                cJSON_AddStringToObject(root_json, "message", "Boundary string u Content-Type zaglavlju je predugacak.");
                goto cleanup_and_send_json; // Skok na čišćenje resursa i slanje JSON odgovora.
            }
            ESP_LOGI(TAG_WEB, "Izdvojeno boundary: '%s'", boundary); // Logira izdvojeni boundary string.
        }
        else
        {
            // Ako "boundary=" nije pronađen, format Content-Type zaglavlja nije očekivan.
            ESP_LOGE(TAG_WEB, "Boundary string nije pronadjen u Content-Type zaglavlju: '%s'", content_type_hdr); // Logira grešku.
            root_json = cJSON_CreateObject();                                                                     // Pripremi JSON grešku.
            cJSON_AddStringToObject(root_json, "status", "error");
            cJSON_AddStringToObject(root_json, "message", "Neispravan Content-Type, nedostaje boundary.");
            goto cleanup_and_send_json; // Skok na čišćenje i slanje greške.
        }
    }
    else
    {
        // Ako Content-Type zaglavlje uopće nije pronađeno ili je predugačko za buffer.
        ESP_LOGE(TAG_WEB, "Content-Type zaglavlje nije pronadjen ili je predugacko."); // Logira grešku.
        root_json = cJSON_CreateObject();                                              // Pripremi JSON grešku.
        cJSON_AddStringToObject(root_json, "status", "error");
        cJSON_AddStringToObject(root_json, "message", "Nedostaje Content-Type zaglavlje.");
        goto cleanup_and_send_json; // Skok na čišćenje i slanje greške.
    }

    ESP_LOGI(TAG_WEB, "Zapocinjem primanje podataka. Ukupno preostalo: %d", remaining); // Logira početak primanja tijela zahtjeva.
    int original_content_len = req->content_len;                                        // Pohrani originalnu duljinu sadržaja za referencu.

    // Glavna petlja za primanje tijela HTTP zahtjeva u chunkovima (dijelovima).
    // Petlja nastavlja primati dok god ima preostalih bajtova (remaining > 0).
    while (remaining > 0)
    {
        ESP_LOGI(TAG_WEB, "Petlja za primanje podataka. Preostalo: %d", remaining); // Logira status u petlji.
        // Prima sljedeći chunk podataka u buffer 'buf'. Čita minimalno od preostalih bajtova ili UPLOAD_BUFFER_SIZE - 1.
        // Ostavljamo 1 bajt slobodan u bufferu kako bismo mogli null-terminirati pročitani chunk za sigurno string operacije (strstr, strchr).
        ret = httpd_req_recv(req, buf, MIN(remaining, UPLOAD_BUFFER_SIZE - 1));
        // Provjerava rezultat funkcije primanja.
        if (ret <= 0)
        {
            // Ako dođe do greške pri primanju (timeout, prekid veze od klijenta, drugi socket error),
            // logira specifičnu grešku i pripremi odgovarajući JSON odgovor o grešci.
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            {
                ESP_LOGE(TAG_WEB, "Timeout pri primanju podataka. Upload nepotpun.");
                root_json = cJSON_CreateObject();
                cJSON_AddStringToObject(root_json, "status", "error");
                cJSON_AddStringToObject(root_json, "message", "Timeout pri primanju podataka tijekom uploada.");
            }
            else if (ret == 0 && remaining > 0)
            {
                ESP_LOGW(TAG_WEB, "Veza prekinuta od strane klijenta tijekom primanja, a ocekivano je jos %d bajtova.", remaining);
                root_json = cJSON_CreateObject();
                cJSON_AddStringToObject(root_json, "status", "error");
                cJSON_AddStringToObject(root_json, "message", "Veza prekinuta od klijenta.");
            }
            else
            { // General socket error
                ESP_LOGE(TAG_WEB, "Greska pri primanju podataka: %d. Upload nepotpun.", ret);
                root_json = cJSON_CreateObject();
                cJSON_AddStringToObject(root_json, "status", "error");
                cJSON_AddStringToObject(root_json, "message", "Greska pri primanju podataka tijekom uploada.");
            }
            goto cleanup_and_send_json; // Skok na dio za čišćenje resursa i slanje JSON odgovora.
        }
        buf[ret] = '\0';                                 // Null-terminiraj primljeni chunk kako bi bio važeći C string.
        ESP_LOGI(TAG_WEB, "Primljeno %d bajtova.", ret); // Logira koliko bajtova je primljeno u ovom chunku.

        // --- Logika za parsiranje multipart/form-data ---
        // Prvi dio multipart poruke sadrži zaglavlja dijela (uključujući Content-Disposition s imenom filea),
        // a nakon praznog reda (\r\n\r\n) počinju stvarni binarni podaci filea.

        if (!file_data_started)
        {
            // Ako još nismo pronašli početak podataka datoteke (tj. obrađujemo prvi dio multipart poruke).
            ESP_LOGI(TAG_WEB, "Podaci datoteke jos nisu poceli. Parsiram zaglavlja u trenutnom dijelu...");
            if (!filename)
            {
                // Ako ime datoteke još nije izdvojeno (traži se u zaglavljima prvog dijela).
                char *cd_header = strstr(buf, "Content-Disposition:"); // Traži standardno zaglavlje Content-Disposition.
                if (cd_header)
                {
                    char *filename_ptr_start = strstr(cd_header, "filename=\""); // Traži "filename=" unutar tog zaglavlja.
                    if (filename_ptr_start)
                    {
                        filename_ptr_start += strlen("filename=\"");              // Pomakni pointer na početak stvarne vrijednosti imena datoteke.
                        char *filename_ptr_end = strchr(filename_ptr_start, '"'); // Traži završni navodnik (").
                        if (filename_ptr_end && filename_ptr_end > filename_ptr_start)
                        {
                            // Ako je ime datoteke pronađeno između navodnika.
                            int len = filename_ptr_end - filename_ptr_start; // Izračunaj duljinu imena.
                            if (len > 128)
                            { // Ograniči maksimalnu duljinu imena datoteke radi sigurnosti.
                                ESP_LOGW(TAG_WEB, "Filename too long (%d chars). Limiting to 128.", len);
                                len = 128;
                            }
                            filename = malloc(len + 1); // Alociraj memoriju za spremanje imena datoteke.
                            if (filename)
                            {
                                strncpy(filename, filename_ptr_start, len); // Kopiraj ime datoteke u alocirani buffer.
                                filename[len] = '\0';                       // Null-terminiraj string.
                                ESP_LOGI(TAG_WEB, "Izdvojeno ime datoteke: '%s' (duljina: %d)", filename, len);
                                // Opcionalno: Ispiši hex dump imena za debug, korisno za provjeru kodiranja.
                                ESP_LOG_BUFFER_HEXDUMP(TAG_WEB, filename, len + 1, ESP_LOG_INFO);

                                // Izgradi punu putanju do datoteke na SD kartici gdje će se file spremiti.
                                // Koristi build_filepath za sigurnost.
                                build_filepath(filepath, sizeof(filepath), MOUNT_POINT, filename);
                                ESP_LOGI(TAG_WEB, "Target file path for upload: '%s'", filepath);

                                // Provjeri postoji li query parametar "overwrite=true" u originalnom URL-u zahtjeva.
                                // Ovo omogućava klijentu da zatraži prepisivanje postojeće datoteke.
                                char overwrite_query_val[10];                            // Buffer za vrijednost parametra (očekuje "true").
                                bool should_overwrite = false;                           // Zastavica za prepisivanje.
                                size_t query_len = httpd_req_get_url_query_len(req) + 1; // Duljina query stringa.
                                if (query_len > 1)
                                {                                              // Ako uopće postoji query string.
                                    char *query_buf_local = malloc(query_len); // Alociraj privremeni buffer za query string.
                                    if (query_buf_local)
                                    {
                                        if (httpd_req_get_url_query_str(req, query_buf_local, query_len) == ESP_OK)
                                        {
                                            // Pokušaj dohvatiti vrijednost parametra "overwrite".
                                            if (httpd_query_key_value(query_buf_local, "overwrite", overwrite_query_val, sizeof(overwrite_query_val)) == ESP_OK)
                                            {
                                                // Usporedi vrijednost s "true" bez obzira na velika/mala slova.
                                                if (strcasecmp(overwrite_query_val, "true") == 0)
                                                {
                                                    should_overwrite = true;                                            // Ako je "true", postavi zastavicu.
                                                    ESP_LOGI(TAG_WEB, "Overwrite flag detected from query parameter."); // Logira detekciju zastavice.
                                                }
                                            }
                                        }
                                        free(query_buf_local); // Oslobodi privremeni buffer.
                                    }
                                    else
                                    {
                                        ESP_LOGE(TAG_WEB, "Failed to allocate local query buffer for overwrite check."); // Logira grešku alokacije.
                                    }
                                }

                                // Provjeri postoji li datoteka na ciljanoj putanji na SD kartici.
                                struct stat st;                        // Struktura za status file-a.
                                int file_exists = stat(filepath, &st); // stat vraća 0 ako datoteka postoji, -1 inače.

                                // Ako datoteka već postoji (file_exists == 0) I prepisivanje NIJE zatraženo.
                                if (file_exists == 0 && !should_overwrite)
                                {
                                    ESP_LOGW(TAG_WEB, "File '%s' already exists. Overwrite not requested.", filename); // Logira upozorenje o konfliktu.
                                    root_json = cJSON_CreateObject();                                                  // Pripremi JSON objekt za odgovor "Conflict".
                                    httpd_resp_set_status(req, "409 Conflict");                                        // Postavi HTTP status kod 409 Conflict.
                                    cJSON_AddStringToObject(root_json, "status", "conflict");                          // Dodaj status u JSON.
                                    char conflict_msg[128 + 100];                                                      // Buffer za poruku o konfliktu.
                                    snprintf(conflict_msg, sizeof(conflict_msg), "Datoteka '%s' već postoji. Želite li je prepisati?", filename);
                                    cJSON_AddStringToObject(root_json, "message", conflict_msg); // Dodaj poruku u JSON.
                                    cJSON_AddStringToObject(root_json, "filename", filename);    // Dodaj ime datoteke u odgovor.

                                    // Važno: Potroši preostale podatke iz tijela zahtjeva kako bi se HTTP veza ispravno zatvorila.
                                    // Ako se ovo ne učini, server može ostati u stanju čekanja na podatke.
                                    ESP_LOGW(TAG_WEB, "Consuming remaining %d bytes of request body due to conflict.", remaining);
                                    char dummy_buf[128]; // Koristi mali dummy buffer.
                                    while (remaining > 0)
                                    { // Čitaj preostale bajtove.
                                        int read_now = httpd_req_recv(req, dummy_buf, MIN(remaining, sizeof(dummy_buf)));
                                        if (read_now <= 0)
                                        { // Prekini ako dođe do greške pri čitanju.
                                            ESP_LOGE(TAG_WEB, "Error draining socket after conflict: %d", read_now);
                                            break;
                                        }
                                        remaining -= read_now; // Smanji brojač preostalih bajtova.
                                        if (remaining < 0)
                                            remaining = 0; // Osiguraj da ne postane negativan.
                                    }
                                    ESP_LOGI(TAG_WEB, "Finished consuming request body.");

                                    goto cleanup_and_send_json; // Skok na dio za čišćenje resursa (oslobađanje filename) i slanje JSON odgovora.
                                }
                                else
                                {
                                    // Ako datoteka ne postoji ILI je prepisivanje zatraženo.
                                    ESP_LOGI(TAG_WEB, "Opening file '%s' for writing (mode 'wb'). File exists: %d, Overwrite requested: %d",
                                             filepath, (file_exists == 0), should_overwrite);

                                    // Pokušaj otvoriti datoteku na SD kartici za pisanje u binarnom modu ('wb').
                                    // 'wb' mod će kreirati novu datoteku ako ne postoji, ili obrisati sadržaj postojeće datoteke ako postoji.
                                    fd = fopen(filepath, "wb"); // fd će biti NULL ako otvaranje ne uspije.
                                    // Provjeri uspješnost otvaranja.
                                    if (!fd)
                                    {
                                        // Ako otvaranje ne uspije, logira grešku (uključujući sistemsku grešku iz errno).
                                        ESP_LOGE(TAG_WEB, "Greska pri otvaranju datoteke za pisanje: '%s' (%s)", filepath, strerror(errno));
                                        root_json = cJSON_CreateObject(); // Pripremi JSON grešku.
                                        cJSON_AddStringToObject(root_json, "status", "error");
                                        // Dodaj detaljniju poruku greške u JSON ako je ime filea poznato.
                                        if (filename)
                                        {
                                            char err_msg[128 + 100];
                                            snprintf(err_msg, sizeof(err_msg), "Nije moguce otvoriti datoteku '%s' za pisanje. Greška: %s", filename, strerror(errno));
                                            cJSON_AddStringToObject(root_json, "message", err_msg);
                                        }
                                        else
                                        { // Fallback poruka ako ime filea nije bilo izdvojeno.
                                            cJSON_AddStringToObject(root_json, "message", "Nije moguce otvoriti datoteku za pisanje.");
                                        }
                                        goto cleanup_and_send_json; // Skok na čišćenje i slanje greške.
                                    }
                                    else
                                    {
                                        ESP_LOGI(TAG_WEB, "Datoteka '%s' uspjesno otvorena za pisanje.", filepath); // Logira uspjeh otvaranja.
                                    }
                                }
                            }
                            else
                            { // malloc failed for filename
                                // Ako alokacija memorije za spremanje imena datoteke ne uspije.
                                ESP_LOGE(TAG_WEB, "Greska pri alokaciji memorije za ime datoteke.");
                                root_json = cJSON_CreateObject(); // Pripremi JSON grešku.
                                cJSON_AddStringToObject(root_json, "status", "error");
                                cJSON_AddStringToObject(root_json, "message", "Interna greska servera (memorija za ime filea).");
                                goto cleanup_and_send_json; // Skok na čišćenje i slanje greške.
                            }
                        }
                        else
                        { // filename="..." not found
                            // Ako zavšni navodnik za ime datoteke nije pronađen u Content-Disposition zaglavlju, format je neispravan.
                            ESP_LOGW(TAG_WEB, "Nije pronadjen zavrsni navodnik para filename=\"...\".");
                            root_json = cJSON_CreateObject(); // Pripremi JSON grešku.
                            cJSON_AddStringToObject(root_json, "status", "error");
                            cJSON_AddStringToObject(root_json, "message", "Neispravan format zaglavlja uploada (ime filea nije parsirano).");
                            goto cleanup_and_send_json; // Skok na čišćenje i slanje greške.
                        }
                    }
                    else
                    { // "filename=" not found
                        // Ako "filename=" nije pronađen u Content-Disposition zaglavlju, format je neispravan.
                        ESP_LOGW(TAG_WEB, "'filename=\"' nije pronadjen u Content-Disposition zaglavlju.");
                        root_json = cJSON_CreateObject(); // Pripremi JSON grešku.
                        cJSON_AddStringToObject(root_json, "status", "error");
                        cJSON_AddStringToObject(root_json, "message", "Ime datoteke nije pronadjeno u Content-Disposition zaglavlju.");
                        goto cleanup_and_send_json; // Skok na čišćenje i slanje greške.
                    }
                }
                else
                { // Content-Disposition: not found
                    // Ako Content-Disposition zaglavlje uopće nije pronađeno (trebalo bi biti u prvom dijelu multipart poruke).
                    ESP_LOGW(TAG_WEB, "Content-Disposition zaglavlje nije pronadjen u multipart dijelu.");
                    root_json = cJSON_CreateObject(); // Pripremi JSON grešku.
                    cJSON_AddStringToObject(root_json, "status", "error");
                    cJSON_AddStringToObject(root_json, "message", "Nedostaje Content-Disposition zaglavlje.");
                    goto cleanup_and_send_json; // Skok na čišćenje i slanje greške.
                }
            }

            // Nakon što smo (potencijalno) pronašli zaglavlja i izdvojili ime datoteke, tražimo početak podataka datoteke.
            // Stvarni podaci datoteke počinju nakon prvog pojavljivanja sekvence CRLF CRLF (\r\n\r\n)
            // koja razdvaja zaglavlja dijela od tijela dijela.
            char *data_start_ptr = strstr(buf, "\r\n\r\n");
            if (data_start_ptr)
            {
                data_start_ptr += 4; // Pomakni pointer za 4 znaka da preskoči "\r\n\r\n".
                ESP_LOGI(TAG_WEB, "Oznaka pocetka podataka (CRLFCRLF) pronadjena.");

                // Provjeri je li file descriptor validan (je li datoteka uspješno otvorena) i je li ime datoteke izdvojeno.
                if (fd && filename)
                {
                    // Izračunaj koliko bajtova u *ovom* chunku predstavlja stvarne podatke datoteke.
                    size_t data_to_write_in_this_chunk = ret - (data_start_ptr - buf);
                    // Traži boundary string *unutar* podataka ovog chunka.
                    // Boundary označava kraj *cijele* multipart poruke ili kraj dijela.
                    // U slučaju uploada jedne datoteke, boundary nakon podataka datoteke je kraj uploada.
                    char *boundary_in_data_ptr = strstr(data_start_ptr, boundary);
                    size_t actual_data_len = data_to_write_in_this_chunk; // Inicijalno, cijeli preostali dio chunka.

                    if (boundary_in_data_ptr)
                    {
                        // Ako je boundary pronađen u ovom chunku, podaci datoteke u ovom chunku završavaju prije boundary stringa.
                        actual_data_len = boundary_in_data_ptr - data_start_ptr; // Izračunaj stvarnu duljinu podataka prije boundary-a.
                        // Oduzmi završni CRLF koji se nalazi *prije* boundary stringa u standardnom multipart formatu.
                        // Provjeri ima li barem 2 bajta prije boundary-a kako bi se izbjegao negativan rezultat.
                        if (actual_data_len >= 2 && *(boundary_in_data_ptr - 1) == '\n' && *(boundary_in_data_ptr - 2) == '\r')
                        {
                            actual_data_len -= 2;
                        }
                        else if (actual_data_len >= 1 && *(boundary_in_data_ptr - 1) == '\n')
                        {
                            // Handle case where it might be just LF (less standard but possible).
                            actual_data_len -= 1;
                        }
                        ESP_LOGI(TAG_WEB, "Boundary pronadjen u prvom data chunku."); // Logira pronalazak boundary-a.
                        remaining = 0;                                                // Postavi preostale bajtove na 0 jer je ovo bio zadnji dio podataka.
                    }

                    if (actual_data_len > 0)
                    {
                        // Ako ima stvarnih podataka za pisanje (duljina > 0), zapiši ih u otvorenu datoteku na SD kartici.
                        fwrite(data_start_ptr, 1, actual_data_len, fd);
                        ESP_LOGI(TAG_WEB, "Zapisano %d bajtova (iz prvog data chunka).", actual_data_len); // Logira koliko bajtova je zapisano.
                    }
                    else if (boundary_in_data_ptr)
                    {
                        // Ako je boundary odmah nakon CRLF CRLF (actual_data_len je 0), to znači da je uploadana prazna datoteka.
                        ESP_LOGI(TAG_WEB, "Boundary je na samom pocetku podataka u ovom chunku (0 bajtova filea prije njega).");
                    }

                    if (remaining == 0)
                    {
                        // Ako je remaining 0, to znači da je boundary pronađen i ovo je bio zadnji chunk podataka.
                        ESP_LOGI(TAG_WEB, "Obrada uploada zavrsena (boundary u prvom data chunku).");
                    }
                    else
                    {
                        // Inače, nastavi s petljom za primanje sljedećih chunkova podataka datoteke.
                        ESP_LOGI(TAG_WEB, "Nastavljam primati sljedece data chunkove.");
                    }
                }
                else if (filename && !fd)
                {
                    // Ovo se ne bi smjelo dogoditi u normalnom toku ako je parsiranje imena bilo uspješno,
                    // ali file nije mogao biti otvoren. Služi kao fallback za grešku.
                    ESP_LOGW(TAG_WEB, "Oznaka pocetka podataka pronadjena, ali fd je NULL! Odbacujem primljene podatke.");
                    // Potroši ostatak zahtjeva ako se pronađe boundary unutar trenutnog chunka.
                    char *boundary_in_chunk = strstr(data_start_ptr, boundary);
                    if (boundary_in_chunk)
                        remaining = 0;
                }
                else if (!filename)
                {
                    // Ako početak podataka pronađen, ali ime datoteke nije bilo izdvojeno (označava grešku parsiranja zaglavlja).
                    ESP_LOGE(TAG_WEB, "Oznaka pocetka podataka pronadjena, ali ime datoteke nije bilo izdvojeno!");
                    root_json = cJSON_CreateObject(); // Pripremi JSON grešku.
                    cJSON_AddStringToObject(root_json, "status", "error");
                    cJSON_AddStringToObject(root_json, "message", "Ime datoteke nije pronadjeno u zaglavlju uploada.");
                    goto cleanup_and_send_json; // Skok na čišćenje i slanje greške.
                }
                file_data_started = true; // Postavi zastavicu da indicira da su podaci datoteke počeli.
            }
            else
            {
                // Ako početak podataka (sekvenca \r\n\r\n) još nije pronađen u ovom chunku.
                // Ovo se može dogoditi ako su zaglavlja prvog dijela multipart poruke veća od UPLOAD_BUFFER_SIZE.
                ESP_LOGI(TAG_WEB, "Oznaka pocetka podataka (CRLFCRLF) jos nije pronadjena u ovom chunku.");
                // U ovom slučaju, ovaj chunk vjerojatno još uvijek sadrži zaglavlja prvog dijela. Podaci se ne pišu u file.
                // TODO: Implementirati robustnije parsiranje multipart zaglavlja koja se protežu preko više chunkova.
            }
        }
        else if (fd)
        {
            // Ako su podaci datoteke već počeli (file_data_started je true) i file descriptor je validan.
            // Obrađujemo sljedeće chunkove koji sadrže samo binarne podatke filea.
            ESP_LOGI(TAG_WEB, "Podaci datoteke su vec poceli. Obrada sljedeceg chunka...");
            // Traži boundary string unutar *cijelog* ovog chunka.
            char *boundary_in_chunk = strstr(buf, boundary);
            size_t data_to_write = ret; // Početno, cijeli primljeni chunk se smatra podacima filea.

            if (boundary_in_chunk)
            {
                // Ako je boundary pronađen u ovom chunku, to je kraj podataka datoteke.
                data_to_write = boundary_in_chunk - buf; // Podaci za pisanje su od početka buffera do boundary stringa.
                                                         // Oduzmi završni CRLF prije boundary.
                if (data_to_write >= 2 && *(boundary_in_chunk - 1) == '\n' && *(boundary_in_chunk - 2) == '\r')
                {
                    data_to_write -= 2;
                }
                else if (data_to_write >= 1 && *(boundary_in_chunk - 1) == '\n')
                {
                    // Handle case where it might be just LF
                    data_to_write -= 1;
                }
                ESP_LOGI(TAG_WEB, "Boundary pronadjen u sljedeceem chunku."); // Logira pronalazak boundary-a.
                remaining = 0;                                                // Postavi preostale bajtove na 0, jer je ovo bio zadnji chunk podataka.
            }

            if (data_to_write > 0)
            {
                // Ako ima podataka za pisanje, zapiši ih u datoteku.
                fwrite(buf, 1, data_to_write, fd);
                ESP_LOGI(TAG_WEB, "Zapisano %d bajtova (iz sljedeceg chunka).", data_to_write); // Logira koliko bajtova je zapisano.
            }
            else if (boundary_in_chunk)
            {
                // Ako je boundary na samom početku chunka (nakon prethodnog chunk-a koji je završio s CRLF).
                ESP_LOGI(TAG_WEB, "Boundary je na samom pocetku sljedeceg chunka (0 bajtova filea prije njega).");
            }

            if (remaining == 0)
            {
                // Ako je remaining 0, to znači da je boundary pronađen i ovo je bio zadnji chunk datoteke.
                ESP_LOGI(TAG_WEB, "Obrada uploada zavrsena (boundary u sljedeceem chunku).");
            }
        }
        else if (filename)
        {
            // Ovo se ne bi smjelo dogoditi - podaci su počeli, ime poznato, ali file descriptor je NULL.
            // Označava internu grešku ili problem s file sustavom.
            ESP_LOGW(TAG_WEB, "File data started, but fd is NULL! Discarding received data.");
            // Pokušaj potrošiti ostatak zahtjeva ako se pronađe boundary.
            char *boundary_in_chunk = strstr(buf, boundary);
            if (boundary_in_chunk)
                remaining = 0;
        }
        else
        {
            // Kritična greška u toku uploada. Nešto je pošlo po zlu u parsiranju ili otvaranju filea.
            ESP_LOGE(TAG_WEB, "Critical error in upload stream - neither filename nor fd are valid after headers.");
            // Pokušaj potrošiti ostatak zahtjeva ako se pronađe boundary.
            char *boundary_in_chunk = strstr(buf, boundary);
            if (boundary_in_chunk)
                remaining = 0;
        }
        remaining -= ret; // Oduzmi broj primljenih bajtova od ukupnog broja preostalih.
        if (remaining < 0)
            remaining = 0; // Osiguraj da remaining ne postane negativan.
    }

    ESP_LOGI(TAG_WEB, "Zavrsena petlja primanja podataka. Konacni 'remaining' (nakon oduzimanja): %d. Originalni content_len: %d", remaining, original_content_len);

// Labela na koju se skače u slučaju greške ili nakon uspješnog završetka petlje primanja.
// Ovdje se vrši čišćenje resursa i slanje konačnog JSON odgovora.
cleanup_and_send_json:
    if (fd)
    {
        fclose(fd); // Zatvori datoteku ako je bila otvorena.
        ESP_LOGI(TAG_WEB, "Datoteka zatvorena.");
    }
    if (buf)
    {
        free(buf); // Oslobađa memoriju alociranu za privremeni buffer za primanje podataka.
        ESP_LOGI(TAG_WEB, "Upload buffer oslobodjen.");
    }

    // Ako root_json objekt još nije kreiran (što znači da nije došlo do greške koja bi ga kreirala prije),
    // kreiraj ga sada i postavi status operacije.
    if (root_json == NULL)
    {
        root_json = cJSON_CreateObject(); // Kreira JSON objekt.
        if (filename && fd)
        { // Ako je ime datoteke poznato I file descriptor je bio validan (sugestija uspjeha).
            ESP_LOGI(TAG_WEB, "Processing of file '%s' completed.", filename);
            char success_msg[128 + 100]; // Buffer za poruku o uspjehu.
            snprintf(success_msg, sizeof(success_msg), "Datoteka '%s' je uspjesno uploadana.", filename);

            cJSON_AddStringToObject(root_json, "status", "success");    // Dodaj status "success".
            cJSON_AddStringToObject(root_json, "message", success_msg); // Dodaj poruku.
            cJSON_AddStringToObject(root_json, "filename", filename);   // Dodaj ime filea u odgovor.
        }
        else
        { // Ako je root_json bio NULL, a filename ili fd nisu validni, označava grešku koja nije specifično obrađena prije.
            ESP_LOGE(TAG_WEB, "Upload finished, but error with filename or file open/processing (after loop).");
            // Dodaj generičku poruku o grešci.
            cJSON_AddStringToObject(root_json, "status", "error");
            cJSON_AddStringToObject(root_json, "message", "Nije bilo moguce obraditi datoteku. Provjerite ime datoteke i dostupnost SD kartice.");
        }
    }

    if (filename)
    {
        free(filename); // Oslobađa memoriju zauzetu za ime datoteke.
        ESP_LOGI(TAG_WEB, "Filename memorija oslobodjena.");
    }

// Labela za konačno slanje JSON odgovora.
send_json_upload_response:
    // Pretvara završni JSON objekt (koji sadrži status i poruku operacije) u formatirani JSON string.
    json_string = cJSON_PrintUnformatted(root_json);
    // Provjerava uspješnost pretvorbe.
    if (json_string == NULL)
    {
        ESP_LOGE(TAG_WEB, "Error converting cJSON object to string for upload response."); // Logira grešku.
        if (root_json)
            cJSON_Delete(root_json); // Pokušaj osloboditi root_json ako postoji.
        // Šalje internu grešku servera ako JSON string ne može biti generiran.
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON print failed after operation");
        return ESP_FAIL; // Vraća ESP_FAIL.
    }
    else
    {
        // Ako je pretvorba uspješna, šalje JSON string kao tijelo HTTP odgovora.
        httpd_resp_send(req, json_string, strlen(json_string));
        ESP_LOGD(TAG_WEB, "Poslan JSON upload odgovor: %s", json_string); // Logira poslani JSON (debug razina).
        cJSON_free(json_string);                                          // Oslobađa memoriju zauzetu za JSON string.
    }

    if (root_json)
        cJSON_Delete(root_json); // Oslobađa memoriju zauzetu za cJSON objekt.

    return ESP_OK; // Vraća ESP_OK (ili ESP_FAIL ako se došlo ovdje nakon kritične greške s JSON-om).
}

// Handler za GET zahtjeve na putanju /log_status.
// Opis: Vraća trenutni status logiranja (aktivno/neaktivno) kao jednostavan JSON objekt.
// Format odgovora: {"active": 1} ako je aktivno, {"active": 0} ako nije.
// NAPOMENA: Postoji i log_status_handler koji vraća plain text, ali ovaj handler (log_status_get_handler)
// je registriran za /log_status URI u start_webserver funkciji.
static esp_err_t log_status_get_handler(httpd_req_t *req)
{
    char resp[32]; // Buffer za JSON odgovor.
    // Dohvaća trenutni status logiranja koristeći thread-safe funkciju is_logging_enabled().
    bool log_active = is_logging_enabled();
    // Formatira odgovor kao JSON objekt s ključem "active" i vrijednošću 1 (true) ili 0 (false).
    int len = snprintf(resp, sizeof(resp), "{\"active\":%d}", log_active ? 1 : 0);

    // Postavlja Content-Type zaglavlje odgovora na 'application/json'.
    httpd_resp_set_type(req, "application/json");
    // Šalje JSON string kao tijelo HTTP odgovora.
    httpd_resp_send(req, resp, len);

    return ESP_OK; // Vraća ESP_OK.
}

// Handler za GET zahtjeve na putanju /settings.html.
// Opis: Poslužuje ugrađeni settings.html file klijentu.
static esp_err_t settings_html_get_handler(httpd_req_t *req)
{
    // Izračunava veličinu ugrađenog HTML file-a.
    size_t html_size = settings_html_end - settings_html_start;
    // Postavlja Content-Type zaglavlje odgovora na 'text/html'.
    httpd_resp_set_type(req, "text/html");
    // Šalje cijeli sadržaj ugrađenog settings.html file-a kao tijelo HTTP odgovora.
    httpd_resp_send(req, (const char *)settings_html_start, html_size);
    return ESP_OK; // Vraća ESP_OK.
}

// Handler koji se (teoretski) aktivira klikom na gumb za pokretanje/zaustavljanje logiranja.
// Opis: Prebacuje status logiranja (sa aktivno na neaktivno i obrnuto).
// Vraća JSON odgovor koji indicira novi status.
// NAPOMENA: Ovaj handler je definiran u kodu, ali NIJE registriran ni na jednu URI putanju
// u funkciji start_webserver(). Vjerojatno je zastario ili se ne koristi u trenutnoj konfiguraciji servera.
// Funkcionalnost prebacivanja logiranja putem web zahtjeva implementirana je u log_toggle_handler.
static esp_err_t toggle_logging_handler(httpd_req_t *req)
{
    // Dohvaća trenutni status logiranja koristeći thread-safe funkciju.
    bool currently_active = is_logging_enabled();
    // Postavlja novi status logiranja na suprotan od trenutnog koristeći thread-safe funkciju.
    set_logging_active(!currently_active);

    // Logira promjenu statusa (informativna razina).
    ESP_LOGI(TAG_WEB, "Logiranje %s", !currently_active ? "UKLJUCENO" : "ISKLJUCENO");

    // Priprema JSON odgovor koji indicira novi status.
    httpd_resp_set_type(req, "application/json"); // Postavlja Content-Type na JSON.
    // Odabire odgovarajući JSON string ovisno o novom statusu.
    const char *response = !currently_active
                               ? "{\"status\":\"enabled\",\"message\":\"Logiranje pokrenuto.\"}"
                               : "{\"status\":\"disabled\",\"message\":\"Logiranje zaustavljeno.\"}";
    // Šalje JSON odgovor.
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK; // Vraća ESP_OK.
}

// Handler za GET zahtjeve na putanju /log.
// Opis: Omogućava daljinsko uključivanje/isključivanje logiranja putem query parametra.
// Očekuje query parametar 'active' s vrijednošću '0' (za isključenje) ili '1' (za uključenje).
// Primjer URL-a: /log?active=1
// Vraća jednostavan JSON odgovor statusa "ok".
static esp_err_t log_toggle_handler(httpd_req_t *req)
{
    char query[16]; // Buffer za primanje query stringa (očekuje kratki string poput "active=1").
    // Dohvaća cijeli query string iz URL-a zahtjeva u buffer.
    esp_err_t ret = httpd_req_get_url_query_str(req, query, sizeof(query));
    // Provjerava je li dohvat query stringa bio uspješan.
    if (ret == ESP_OK)
    {
        char val[4]; // Buffer za vrijednost parametra 'active' (očekuje "0", "1", ili nešto drugo).
        // Pokušava izdvojiti vrijednost parametra "active" iz query stringa.
        if (httpd_query_key_value(query, "active", val, sizeof(val)) == ESP_OK)
        {
            // Pretvara vrijednost u boolean: true ako je string "1", false inače.
            bool active = (strcmp(val, "1") == 0);
            // Postavlja globalni status logiranja koristeći thread-safe funkciju.
            set_logging_active(active);
            // Logira novopostavljeni status (informativna razina).
            ESP_LOGI(TAG_WEB, "Logiranje postavljeno na: %s", active ? "UKLJUCENO" : "ISKLJUCENO");
        }
    }
    // Šalje jednostavan JSON odgovor klijentu koji samo potvrđuje da je zahtjev obrađen.
    httpd_resp_set_type(req, "application/json");      // Postavlja Content-Type na JSON.
    const char *resp = "{\"status\":\"ok\"}";          // Jednostavan JSON string.
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN); // Šalje JSON.
    return ESP_OK;                                     // Vraća ESP_OK.
}

// Handler za GET zahtjeve na putanju /adc.
// Opis: Vraća zadnjih 8 očitanih vrijednosti napona s oba ADS1115 ADC-a u JSON formatu.
// Vrijednosti se dobivaju iz globalne varijable last_voltages.
// Format odgovora: JSON niz float vrijednosti, npr. [1.2345, 0.5678, 3.0102, 0.0000, ...].
esp_err_t adc_handler(httpd_req_t *req)
{
    float voltages[NUM_CHANNELS]; // Lokalno polje za sigurno kopiranje podataka
    // Dohvaćamo i konfiguracije da bismo znali jedinice za svaki kanal.
    const channel_config_t *configs = settings_get_channel_configs();

    // Korištenje mutexa za sigurno čitanje globalnog polja 'last_voltages'.
    if (xSemaphoreTake(logging_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        memcpy(voltages, last_voltages, sizeof(last_voltages));
        xSemaphoreGive(logging_mutex);
    }
    else
    {
        // Ako je mutex zauzet, popuni lokalno polje nulama kao fallback.
        memset(voltages, 0, sizeof(voltages));
    }

    // Kreiranje JSON odgovora pomoću cJSON biblioteke.
    cJSON *root = cJSON_CreateObject();
    cJSON *kanali_array = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "kanali", kanali_array);

    // Petlja kroz svih 8 kanala za kreiranje JSON objekata
    for (int i = 0; i < NUM_CHANNELS; i++)
    {
        cJSON *kanal_obj = cJSON_CreateObject();
        // Vrijednosti u 'voltages' polju su već skalirane u main.c
        cJSON_AddNumberToObject(kanal_obj, "vrijednost", voltages[i]);
        // Dodajemo i mjernu jedinicu iz konfiguracije.
        cJSON_AddStringToObject(kanal_obj, "jedinica", configs[i].unit);
        cJSON_AddItemToArray(kanali_array, kanal_obj);
    }

    // Pretvaranje cJSON strukture u string.
    char *json_string = cJSON_PrintUnformatted(root);
    // Postavljanje HTTP headera da klijent zna da prima JSON.
    httpd_resp_set_type(req, "application/json");
    // Slanje JSON stringa kao odgovora.
    httpd_resp_send(req, json_string, strlen(json_string));

    // Oslobađanje memorije zauzete od strane cJSON-a.
    cJSON_free(json_string);
    cJSON_Delete(root);

    return ESP_OK;
}

/**
 * @brief Handler za GET /api/channel-configs (API) - dohvaća postavke.
 * @param req HTTP zahtjev.
 * @return esp_err_t
 *
 * API endpoint koji dohvaća trenutne konfiguracije svih 8 kanala iz 'settings' modula
 * i šalje ih kao JSON polje. Frontend (JavaScript na settings.html) će koristiti ovo
 * za popunjavanje forme s postojećim vrijednostima prilikom učitavanja stranice.
 */
static esp_err_t channel_configs_get_handler(httpd_req_t *req)
{
    // Dohvati trenutne konfiguracije iz settings modula.
    const channel_config_t *configs = settings_get_channel_configs();

    // Kreiraj korijenski JSON objekt, koji će biti polje (array).
    cJSON *root = cJSON_CreateArray();
    if (!root)
    {
        // Ako alokacija memorije za JSON ne uspije, vrati internu grešku servera.
        return httpd_resp_send_500(req);
    }

    // Prođi kroz svih 8 kanala i za svaki kreiraj JSON objekt.
    for (int i = 0; i < NUM_CHANNELS; i++)
    {
        cJSON *cfg_obj = cJSON_CreateObject();
        if (!cfg_obj)
        {
            cJSON_Delete(root); // Oslobodi prethodno alociranu memoriju.
            return httpd_resp_send_500(req);
        }

        cJSON_AddNumberToObject(cfg_obj, "factor", configs[i].scaling_factor);
        cJSON_AddStringToObject(cfg_obj, "unit", configs[i].unit);
        cJSON_AddItemToArray(root, cfg_obj);
    }

    // Pretvaranje cJSON strukture u string i slanje klijentu.
    char *json_string = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));

    // Oslobađanje memorije.
    cJSON_free(json_string);
    cJSON_Delete(root);

    return ESP_OK;
}



// Handler za GET zahtjeve na putanju /logging.html.
// Opis: Poslužuje ugrađeni logging.html file klijentu.
static esp_err_t logging_html_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WEB, "Serviram /logging.html"); // Logira informaciju.
    // Postavlja Content-Type zaglavlje na 'text/html'.
    httpd_resp_set_type(req, "text/html");
    // Šalje cijeli sadržaj ugrađenog logging.html file-a.
    httpd_resp_send(req, (const char *)logging_html_start, logging_html_end - logging_html_start);
    return ESP_OK; // Vraća ESP_OK.
}

// Handler za dohvat statusa logiranja kao plain text.
// Opis: Vraća samo broj 0 (neaktivno) ili 1 (aktivno) kao čisti tekst.
// NAPOMENA: Ovaj handler je definiran, ali NIJE registriran ni na jednu URI putanju
// u start_webserver funkciji. log_status_get_handler vraća status u JSON formatu i JEST registriran.
static esp_err_t log_status_handler(httpd_req_t *req)
{
    char resp[32];      // Buffer za odgovor.
    int log_status = 0; // Varijabla za status (0 ili 1).

    // Čitanje statusa logiranja koristeći mutex za thread-safe pristup globalnoj varijabli.
    // Koristi timeout od 10ms. Ako ne uspije, log_status ostaje 0 (inicijalna vrijednost).
    if (xSemaphoreTake(logging_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        log_status = logging_active;   // Čita globalnu varijablu logging_active.
        xSemaphoreGive(logging_mutex); // Otpusti mutex.
    }
    else
    {
        // Logira upozorenje ako mutex nije preuzet (ako je potrebno, trenutno zakomentirano).
        // ESP_LOGW(TAG_WEB, "Timeout preuzimanja mutexa za log_status handler.");
    }

    // Formatira odgovor kao broj (0 ili 1) u string.
    int len = snprintf(resp, sizeof(resp), "%d", log_status);

    httpd_resp_set_type(req, "text/plain"); // Postavlja Content-Type na plain text.
    httpd_resp_send(req, resp, len);        // Šalje odgovor.
    return ESP_OK;                          // Vraća ESP_OK.
}

// handler za java script graf
static esp_err_t chart_js_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WEB, "Serving embedded /chart.js");
    httpd_resp_set_type(req, "application/javascript"); // Ispravan MIME tip
    httpd_resp_send(req, (const char *)chart_js_start, chart_js_end - chart_js_start);
    return ESP_OK;
}


/**
 * @brief Implementacija funkcije za dohvat imena trenutno aktivne log datoteke.
 * Sigurno (thread-safe) dohvaća naziv datoteke iz globalne varijable 'g_current_log_filepath'
 * koja se ažurira u 'main.c'.
 * @return const char* Null-terminirani string s imenom datoteke.
 * Vraća "N/A (Mutex Missing)" ili "N/A (Mutex Busy)" ako mutex nije inicijaliziran ili ne može biti preuzet.
 */
const char* get_current_log_file_name(void) { // Ovdje NEMA 'static'
    // Provjeri je li mutex inicijaliziran (ovo bi se trebalo dogoditi u app_main).
    if (g_log_file_path_mutex == NULL) {
        ESP_LOGE(TAG_WEB, "g_log_file_path_mutex je NULL! Vracam default.");
        return "N/A (Mutex Missing)";
    }
    // Pokušaj preuzeti mutex s kratkim timeoutom (npr. 10ms) kako bi se izbjeglo blokiranje.
    if (xSemaphoreTake(g_log_file_path_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        // Ako je mutex preuzet, pročitaj globalnu varijablu.
        const char* current_file_path = g_current_log_filepath;
        xSemaphoreGive(g_log_file_path_mutex); // Otpusti mutex.
        return current_file_path;
    } else {
        // Ako mutex nije preuzet, logiraj upozorenje i vrati defaultnu vrijednost.
        ESP_LOGW(TAG_WEB, "Timeout preuzimanja mutexa za ime trenutne log datoteke. Vracam N/A.");
        return "N/A (Mutex Busy)";
    }
}

/**
 * @brief Handler for GET requests to the `/current_log_file` URI.
 * Returns the name of the currently active log file as plain text.
 * This function retrieves the filename using the `get_current_log_file_name()` utility.
 * @param req Pointer to the HTTP request structure.
 * @return esp_err_t ESP_OK on success.
 */
static esp_err_t current_log_file_handler(httpd_req_t *req)
{
    const char* filename = get_current_log_file_name(); // Dohvati ime datoteke.
    httpd_resp_set_type(req, "text/plain");             // Odgovor je plain text.
    httpd_resp_sendstr(req, filename);                  // Pošalji ime datoteke.
    return ESP_OK;
}


// Funkcija: start_webserver
// Opis: Konfigurira i pokreće HTTP server na ESP32 te registrira sve URI handlere.
// Ova funkcija je glavna ulazna točka za pokretanje web server funkcionalnosti.
// Povratna vrijednost: esp_err_t - ESP_OK ako je server uspješno pokrenut i handleri registrirani, inače ESP_FAIL.
esp_err_t start_webserver()
{
    // Inicijalizacija mutexa za thread-safe pristup globalnim varijablama (logging_active, last_voltages).
    // Kreira se samo ako već nije (npr. pri prvom pokretanju servera).
    if (logging_mutex == NULL)
    {
        logging_mutex = xSemaphoreCreateMutex(); // Pokušaj kreiranja mutexa.
        if (logging_mutex == NULL)
        {
            ESP_LOGE(TAG_WEB, "Greska pri kreiranju logging_mutex!"); // Logira grešku ako kreiranje ne uspije.
            return ESP_FAIL;                                          // Vraća grešku.
        }
        // ESP_LOGI(TAG_WEB, "Logging mutex kreiran."); // Opcionalno: Logira kreiranje mutexa.
    }

    // Inicijalizacija konfiguracijske strukture za HTTP server.
    // HTTPD_DEFAULT_CONFIG() pruža standardne zadane postavke preporučene od strane ESP-IDF-a.
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Prilagodba nekih defaultnih postavki za ovaj specifični server:
    config.stack_size = 16384;    // Povećaj veličinu stoga (stack) za HTTP server task. Web serveri obično trebaju više memorije stoga za obradu zahtjeva. Default je često 4096.
    config.max_uri_handlers = 20; // Povećaj maksimalni broj URI handlera koji se mogu registrirati. Omogućava registraciju više različitih URL putanja. Default je često 8.
    // Povećanje ovih vrijednosti može utjecati na potrošnju RAM-a, pa ih treba prilagoditi potrebama.
    // config.core_id = 0; // Opcionalno: Može se postaviti da server task radi na određenoj jezgri procesora (0 ili 1 na dual-core ESP32). Ostavljanje zadano (tskNO_AFFINITY) dopušta scheduleru da odabere.

    // Postavlja funkciju koja se koristi za uspoređivanje URI-ja dolaznih zahtjeva s registriranim URI-jima handlera.
    // httpd_uri_match_wildcard omogućava korištenje wildcard znakova (npr. /files/*) u URI-jima definicija handlera,
    // iako u ovom kodu nema wildcard handlera, ova postavka ne smeta i može biti korisna za buduće proširenje.
    config.uri_match_fn = httpd_uri_match_wildcard;

    // Logiranje porta na kojem će server pokušati pokrenuti. Port se obično konfigurira u sdkconfig-u projekta.
    ESP_LOGI(TAG_WEB, "Pokrecem HTTP server na portu: %d (Max header len konfiguriran preko menuconfig)",
             config.server_port);

    // Pokretanje HTTP server instance.
    // Funkcija httpd_start() inicijalizira server, stvara potrebne taskove i započinje slušanje na konfiguriranom portu.
    // Argumenti su: pointer na httpd_handle_t varijablu (gdje će se spremiti handler instance) i konfiguracijska struktura.
    if (httpd_start(&server, &config) != ESP_OK)
    {
        // Ako pokretanje servera ne uspije, logiraj grešku.
        ESP_LOGE(TAG_WEB, "Greska pri pokretanju servera!");
        server = NULL;   // Postavi server handler na NULL da indicira da server nije pokrenut.
        return ESP_FAIL; // Vraća ESP_FAIL.
    }

    ESP_LOGI(TAG_WEB, "Registriram URI handlere"); // Logira početak registracije handlera.

    // --- Registracija URI Handlera ---
    // Za svaku putanju (URI) koju server treba obraditi (npr. "/", "/list", "/upload"), kreira se httpd_uri_t struktura
    // koja definira URI, HTTP metodu (GET, POST, itd.), funkciju handlera koja će obraditi zahtjev, i opcionalno korisnički kontekst.
    // Nakon konfiguracije strukture, handler se registrira pomoću httpd_register_uri_handler().

    // Handler za root URI "/" (glavna stranica). Obrada GET zahtjeva.
    httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &root_uri); // Registracija handlera.

    // Handler za URI "/list" (stranica s popisom datoteka na SD kartici). Obrada GET zahtjeva.
    httpd_uri_t list_uri = {.uri = "/list", .method = HTTP_GET, .handler = list_get_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &list_uri);

    // Handler za URI "/download" (preuzimanje datoteka s SD kartice). Obrada GET zahtjeva.
    // Koristi query parametar za ime datoteke.
    httpd_uri_t download_uri = {.uri = "/download", .method = HTTP_GET, .handler = download_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &download_uri);

    // Handler za URI "/delete" (brisanje datoteka s SD kartice). Obrada GET zahtjeva.
    // Koristi query parametar za ime datoteke. Vraća JSON status.
    httpd_uri_t delete_uri = {.uri = "/delete", .method = HTTP_GET, .handler = delete_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &delete_uri);

// Handler za brisanje SVIH datoteka
    httpd_uri_t delete_all_uri = {
        .uri = "/delete_all",
        .method = HTTP_GET, // Ili HTTP_POST za destruktivne akcije
        .handler = delete_all_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &delete_all_uri);

    // Handler za URI "/style.css" (posluživanje ugrađenog CSS file-a). Obrada GET zahtjeva.
    httpd_uri_t css_uri = {.uri = "/style.css", .method = HTTP_GET, .handler = css_get_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &css_uri);

    // Handler za URI "/script.js" (posluživanje ugrađenog JS file-a). Obrada GET zahtjeva.
    httpd_uri_t js_uri = {.uri = "/script.js", .method = HTTP_GET, .handler = js_get_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &js_uri);

    // Handler za URI "/upload" (upload datoteka na SD karticu). Koristi POST metodu.
    // Rukuje multipart/form-data tijelom zahtjeva.
    httpd_uri_t upload_uri = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = upload_post_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &upload_uri);

    // Handler za URI "/logging.html" (posluživanje ugrađene HTML stranice za logiranje/monitoring). Obrada GET zahtjeva.
    httpd_uri_t logging_uri = {
        .uri = "/logging.html",
        .method = HTTP_GET,
        .handler = logging_html_get_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &logging_uri);

    // Handler za URI "/adc" (dohvat zadnjih ADS1115 vrijednosti putem AJAX-a/API-ja). Obrada GET zahtjeva.
    // Vraća JSON niz float vrijednosti.
    httpd_uri_t adc_uri = {
        .uri = "/adc",
        .method = HTTP_GET,
        .handler = adc_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &adc_uri);

    // Handler za URI "/log" (uključivanje/isključivanje logiranja putem query parametra ?active=0/1). Obrada GET zahtjeva.
    // Vraća JSON status.
    httpd_uri_t log_uri = {
        .uri = "/log",
        .method = HTTP_GET,
        .handler = log_toggle_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &log_uri);

    // Handler za GET zahtjeve na URI "/settings" (dohvat postavki logiranja kao JSON). Obrada GET zahtjeva.
    httpd_uri_t get_settings_uri = {
        .uri = "/settings",
        .method = HTTP_GET,
        .handler = settings_get_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &get_settings_uri);

    // Handler za POST zahtjeve na URI "/settings" (postavljanje postavki logiranja putem JSON-a u tijelu zahtjeva). Obrada POST zahtjeva.
    httpd_uri_t post_settings_uri = {
        .uri = "/settings",
        .method = HTTP_POST,
        .handler = settings_post_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &post_settings_uri);

// Registrira handler za POST API za spremanje konfiguracija kanala.
    httpd_uri_t post_channel_configs_uri = {
        .uri       = "/api/channel-configs",
        .method    = HTTP_POST,
        .handler   = channel_configs_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &post_channel_configs_uri);

    // Handler za URI "/settings.html" (posluživanje ugrađene HTML stranice za postavke). Obrada GET zahtjeva.
    httpd_uri_t settings_html = {
        .uri = "/settings.html",
        .method = HTTP_GET,
        .handler = settings_html_get_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &settings_html);

    // Registrira handler za GET API za dohvat konfiguracija kanala.
    httpd_uri_t get_configs_api = {
        .uri = "/api/channel-configs",
        .method = HTTP_GET,
        .handler = channel_configs_get_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &get_configs_api);


    // Handler for embedded Chart.js file
httpd_uri_t chartjs_uri = {
    .uri = "/chart.js", // URI po kojem će preglednik tražiti
    .method = HTTP_GET,
    .handler = chart_js_get_handler, // Referencira novu funkciju
    .user_ctx = NULL
};
httpd_register_uri_handler(server, &chartjs_uri);

     // Handler za URI "/log_status" (dohvat statusa logiranja kao JSON). Obrada GET zahtjeva.
    static const httpd_uri_t log_status_uri = {
        .uri = "/log_status",
        .method = HTTP_GET,
        .handler = log_status_get_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &log_status_uri);

    // NOVO: Handler za dohvat imena trenutne log datoteke
    httpd_uri_t current_log_file_uri = {
        .uri = "/current_log_file",
        .method = HTTP_GET,
        .handler = current_log_file_handler, // Referencira funkciju handlera
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &current_log_file_uri);

    ESP_LOGI(TAG_WEB, "Web server pokrenut."); // Logira informaciju o uspješnom pokretanju.
    return ESP_OK;                             // Vraća ESP_OK ako je server uspješno pokrenut i handleri registrirani.


    
    
}

// Funkcija: stop_webserver
// Opis: Zaustavlja pokrenutu instancu HTTP servera.
// Treba se pozvati pri gašenju sustava ili ako više nije potreban web server.
void stop_webserver()
{
    // Provjerava je li server instanca validna (pointer nije NULL).
    if (server)
    {
        ESP_LOGI(TAG_WEB, "Zaustavljam web server"); // Logira početak zaustavljanja.
        // Poziva funkciju iz ESP-IDF HTTP server komponente za zaustavljanje servera.
        // Ova funkcija gasi slušanje na portu, zatvara aktivne konekcije i oslobađa resurse.
        httpd_stop(server);
        server = NULL;                               // Postavlja server handler na NULL da indicira da server više ne radi.
        ESP_LOGI(TAG_WEB, "Web server zaustavljen"); // Logira završetak zaustavljanja.
    }
    // NAPOMENA: Ovdje se ne oslobađa mutex `logging_mutex` jer bi se server (i s njim povezane funkcije)
    // potencijalno mogao ponovno pokrenuti kasnije. Mutex bi se trebao osloboditi samo ako se zna
    // da se server i svi taskovi koji ga koriste više nikada neće pokrenuti tijekom životnog vijeka aplikacije.
}