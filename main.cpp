// ROM Store - catalogo e downloader para Nintendo Switch (libnx + SDL2 + curl + SDL2_mixer)
//
// Menu:  Consoles -> jogos (alfabeto suspenso com ZL) -> detalhe -> download (com Snake)
// Leve para o Switch: imagens carregadas em segundo plano, reduzidas e com limite de memoria,
// textos em cache, tela so e redesenhada quando muda algo (menos CPU/GPU = menos aquecimento).
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "json.hpp"
using json = nlohmann::json;

// Endereco padrao do catalogo. Pode ser trocado sem recompilar em sdmc:/switch/romstore/config.ini
// (conteudo do arquivo:  catalogo=https://seusite.com/catalogo.json)
static const char* DEFAULT_URL = "https://SEUUSUARIO.github.io/SEUREPO/catalogo.json";
static const char* ROOT_DIR = "sdmc:/switch/romstore";
static const char* CONFIG_PATH = "sdmc:/switch/romstore/config.ini";
static const char* CACHE_DIR = "sdmc:/switch/romstore/cache";
static const char* BG_DIR = "sdmc:/switch/romstore/fundos";  // coloque aqui as imagens de fundo

static const int W = 1280, H = 720, HDR = 76;
static const int COLS = 4, PER = 8;       // grade de jogos: 4 colunas x 2 linhas
static const int ALPHA_COLS = 14;         // alfabeto suspenso: 14 letras por linha
static const Uint32 SPLASH_MS = 4000;     // tempo minimo da tela de loading
static const size_t MAX_IMGS = 56;        // limite de imagens na memoria

// ---------- temas ----------
struct Theme {
    const char* name;
    SDL_Color bg, accent, accentL, sel, card, ink, text, gray, ph;
};
static const Theme THEMES[] = {
    {"Laranja", {24, 24, 30, 255}, {255, 122, 0, 255}, {255, 170, 80, 255}, {0, 170, 255, 255}, {255, 255, 255, 255},
     {25, 25, 30, 255}, {255, 255, 255, 255}, {170, 170, 182, 255}, {58, 58, 70, 255}},
    {"Azul", {14, 22, 38, 255}, {0, 120, 215, 255}, {70, 160, 240, 255}, {255, 200, 0, 255}, {255, 255, 255, 255},
     {20, 25, 35, 255}, {255, 255, 255, 255}, {160, 175, 195, 255}, {45, 58, 82, 255}},
    {"Verde", {16, 30, 24, 255}, {34, 160, 90, 255}, {90, 200, 140, 255}, {255, 214, 10, 255}, {255, 255, 255, 255},
     {20, 30, 25, 255}, {255, 255, 255, 255}, {160, 185, 170, 255}, {44, 66, 54, 255}},
    {"Roxo", {26, 18, 40, 255}, {140, 70, 220, 255}, {180, 120, 240, 255}, {0, 220, 200, 255}, {255, 255, 255, 255},
     {28, 20, 40, 255}, {255, 255, 255, 255}, {180, 165, 200, 255}, {62, 48, 84, 255}},
    {"Vermelho", {28, 16, 16, 255}, {210, 50, 50, 255}, {240, 110, 110, 255}, {255, 210, 60, 255}, {255, 255, 255, 255},
     {35, 20, 20, 255}, {255, 255, 255, 255}, {195, 165, 165, 255}, {80, 46, 46, 255}},
    {"Claro", {235, 236, 242, 255}, {255, 122, 0, 255}, {255, 170, 80, 255}, {0, 120, 220, 255}, {255, 255, 255, 255},
     {25, 25, 30, 255}, {30, 30, 40, 255}, {105, 105, 120, 255}, {205, 205, 215, 255}},
};
static const int NTHEMES = (int)(sizeof(THEMES) / sizeof(THEMES[0]));
static const Theme* T = &THEMES[0];
static const SDL_Color ONACC = {255, 255, 255, 255}, GREEN = {50, 170, 90, 255};

static SDL_Renderer* ren;
static TTF_Font *fS, *fM, *fL;
static unsigned frameNo = 0;            // quadros desenhados (usado nos caches)
static std::atomic<unsigned> gFrame{0};

// ---------- dados ----------
struct Game {
    std::string id, titulo, sistema, descricao, capa, url, arquivo, tamanho, tl, sl;  // tl/sl: titulo e sistema em minusculas
    std::vector<std::string> fotos;
    char letter = '#';  // primeira letra do titulo, reconhecida automaticamente
};
struct Console {
    std::string nome, imagem;
    int count = 0;
};
static std::vector<Game> games;
static std::vector<Console> consoles = {{"N64", "", 0}, {"PSP", "", 0}, {"PS1", "", 0}, {"DS", "", 0}};
static std::vector<int> view;
static std::vector<std::string> letters;
static std::string query, curLetter = "Todos", loadError, catalogBase;
static std::string audAbertura, audClique, audFundo, audVersao;
static std::string pAbertura, pClique, pFundo;  // arquivos de som ja baixados
static int ci = 0;  // console aberto

// ---------- utilidades ----------
static std::string lower(std::string s) { for (auto& c : s) c = tolower((unsigned char)c); return s; }
static bool safeName(const std::string& s) {
    return !s.empty() && s.find('/') == std::string::npos && s.find('\\') == std::string::npos && s.find("..") == std::string::npos;
}
static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static bool isUrl(const std::string& s) { return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0; }
static void mkdirs(const std::string& path) {  // cria a pasta e todas as anteriores
    size_t i = path.find('/');
    while (i != std::string::npos) {
        mkdir(path.substr(0, i).c_str(), 0777);
        i = path.find('/', i + 1);
    }
    mkdir(path.c_str(), 0777);
}
static std::string baseName(const std::string& p) {
    size_t k = p.rfind('/');
    return k == std::string::npos ? p : p.substr(k + 1);
}
static char accentBase(unsigned char d) {  // letra base de um caractere latino acentuado (UTF-8: 0xC3 d)
    if ((d >= 0x80 && d <= 0x85) || (d >= 0xA0 && d <= 0xA5)) return 'A';
    if (d == 0x87 || d == 0xA7) return 'C';
    if ((d >= 0x88 && d <= 0x8B) || (d >= 0xA8 && d <= 0xAB)) return 'E';
    if ((d >= 0x8C && d <= 0x8F) || (d >= 0xAC && d <= 0xAF)) return 'I';
    if (d == 0x91 || d == 0xB1) return 'N';
    if ((d >= 0x92 && d <= 0x96) || d == 0x98 || (d >= 0xB2 && d <= 0xB6) || d == 0xB8) return 'O';
    if ((d >= 0x99 && d <= 0x9C) || (d >= 0xB9 && d <= 0xBC)) return 'U';
    return 0;
}
static char letterOf(const std::string& s) {  // 'A'..'Z' ou '#' (numeros/simbolos)
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        if (c < 0x80) {
            if (isalpha(c)) return (char)toupper(c);
            if (isdigit(c)) return '#';
            i++;
        } else if (c == 0xC3 && i + 1 < s.size()) {
            char r = accentBase((unsigned char)s[i + 1]);
            if (r) return r;
            i += 2;
        } else i++;
    }
    return '#';
}
static unsigned long long fnv(const std::string& s) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
static std::string resolveUrl(const std::string& u) {  // caminho relativo = relativo ao catalogo
    if (u.empty() || isUrl(u)) return u;
    return catalogBase + u;
}

// ---------- configuracao ----------
static std::map<std::string, std::string> cfg;
static void loadConfig() {
    if (FILE* f = fopen(CONFIG_PATH, "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            size_t eq = s.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            cfg[s.substr(0, eq)] = s.substr(eq + 1);
        }
        fclose(f);
    }
}
static void saveConfig() {
    mkdirs(ROOT_DIR);
    if (FILE* f = fopen(CONFIG_PATH, "w")) {
        for (auto& kv : cfg) fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
        fclose(f);
    }
}
static int cfgInt(const std::string& k, int def) {
    auto it = cfg.find(k);
    return it == cfg.end() || it->second.empty() ? def : atoi(it->second.c_str());
}
static std::string cfgStr(const std::string& k) {
    auto it = cfg.find(k);
    return it == cfg.end() ? "" : it->second;
}
static void cfgSet(const std::string& k, const std::string& v) { cfg[k] = v; saveConfig(); }
static void setTheme(int i) { T = &THEMES[((i % NTHEMES) + NTHEMES) % NTHEMES]; }

// ---------- pasta das ROMs (escolhida por console, vale para todas as letras) ----------
static std::string defaultDir(const std::string& sys) { return "sdmc:/roms/" + lower(sys); }
static std::string romDir(const std::string& sys) { return cfgStr("rom_" + lower(sys)); }
static std::string joinPath(const std::string& d, const std::string& n) { return !d.empty() && d.back() == '/' ? d + n : d + "/" + n; }
static std::string destPath(const Game& g) {
    std::string d = romDir(g.sistema);
    return joinPath(d.empty() ? defaultDir(g.sistema) : d, g.arquivo);
}

// ---------- rede ----------
static void setup(CURL* c, const std::string& url) {
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);  // v1: sem certificado CA embutido
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "romstore/1.0");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);   // desiste se ficar parado
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
}
static size_t wMem(char* p, size_t s, size_t n, void* u) { ((std::string*)u)->append(p, s * n); return s * n; }
static size_t wFile(char* p, size_t s, size_t n, void* u) { return fwrite(p, s, n, (FILE*)u); }

static bool fetch(const std::string& url, std::string& out) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    setup(c, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wMem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    return r == CURLE_OK && code >= 200 && code < 300;
}
// Baixa uma vez e guarda no cartao SD (menos rede = menos aquecimento). A chave inclui o "versao" do catalogo:
// para forcar a troca de um som/imagem mantendo o mesmo nome de arquivo, aumente o "versao" no catalogo.json.
static std::string cachedFile(const std::string& url, const std::string& ver) {
    if (url.empty()) return "";
    std::string ext;
    std::string clean = url.substr(0, url.find('?'));
    size_t dot = clean.rfind('.');
    if (dot != std::string::npos && clean.size() - dot <= 5 && clean.find('/', dot) == std::string::npos) ext = lower(clean.substr(dot));
    char key[32];
    snprintf(key, sizeof key, "%016llx", fnv(url + "|" + ver));
    std::string path = std::string(CACHE_DIR) + "/" + key + ext;
    if (exists(path)) return path;
    std::string data;
    if (!fetch(url, data) || data.empty()) return "";
    mkdirs(CACHE_DIR);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "";
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return path;
}

// ---------- audio ----------
static Mix_Chunk *sClick = nullptr, *sOpen = nullptr;
static Mix_Music* bgm = nullptr;
static bool audioOk = false;

static void prefetchAudio() {  // roda durante o loading
    pClique = cachedFile(resolveUrl(audClique), audVersao);
    pAbertura = cachedFile(resolveUrl(audAbertura), audVersao);
    pFundo = cachedFile(resolveUrl(audFundo), audVersao);
}
static void playClick() { if (audioOk && sClick && cfgInt("sfx", 1)) Mix_PlayChannel(-1, sClick, 0); }
static void applyMusic() {
    if (!audioOk || !bgm) return;
    if (cfgInt("musica", 1)) {
        if (Mix_PausedMusic()) Mix_ResumeMusic();
        else if (!Mix_PlayingMusic()) Mix_FadeInMusic(bgm, -1, 1500);
    } else Mix_PauseMusic();
}
static void loadAudio() {
    Mix_Init(MIX_INIT_OGG | MIX_INIT_MP3);
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) != 0) return;
    audioOk = true;
    if (!pClique.empty()) sClick = Mix_LoadWAV(pClique.c_str());
    if (!pAbertura.empty()) sOpen = Mix_LoadWAV(pAbertura.c_str());
    if (!pFundo.empty()) bgm = Mix_LoadMUS(pFundo.c_str());
    Mix_VolumeMusic(MIX_MAX_VOLUME / 2);
    if (sClick) Mix_VolumeChunk(sClick, MIX_MAX_VOLUME * 3 / 4);
    if (sOpen && cfgInt("sfx", 1)) Mix_PlayChannel(-1, sOpen, 0);
    applyMusic();
}

// ---------- imagens: carregadas em segundo plano, reduzidas e com limite de memoria ----------
// Decodifica e reduz (media de pixels) para no maximo mw x mh. Roda na thread de imagens.
static SDL_Surface* loadScaled(const std::string& path, int mw, int mh) {
    SDL_Surface* s0 = IMG_Load(path.c_str());
    if (!s0) return nullptr;
    if ((long long)s0->w * s0->h > 8000000LL) { SDL_FreeSurface(s0); return nullptr; }  // imagem enorme: recusa (evita estourar a memoria)
    SDL_Surface* s = SDL_ConvertSurfaceFormat(s0, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s0);
    if (!s) return nullptr;
    if (s->w <= mw && s->h <= mh) return s;
    float sc = std::min((float)mw / s->w, (float)mh / s->h);
    int dw = std::max(1, (int)(s->w * sc)), dh = std::max(1, (int)(s->h * sc));
    SDL_Surface* d = SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!d) { SDL_FreeSurface(s); return nullptr; }
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long long)y * s->h / dh), y1 = std::max(y0 + 1, (int)((long long)(y + 1) * s->h / dh));
        int sy = std::max(1, (y1 - y0) / 4);
        Uint32* out = (Uint32*)((Uint8*)d->pixels + y * d->pitch);
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * s->w / dw), x1 = std::max(x0 + 1, (int)((long long)(x + 1) * s->w / dw));
            int sx = std::max(1, (x1 - x0) / 4);
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < s->h; yy += sy) {
                const Uint32* row = (const Uint32*)((const Uint8*)s->pixels + yy * s->pitch);
                for (int xx = x0; xx < x1 && xx < s->w; xx += sx) {
                    Uint32 p = row[xx];
                    r += p & 0xFF; g += (p >> 8) & 0xFF; b += (p >> 16) & 0xFF; a += p >> 24; n++;
                }
            }
            if (!n) n = 1;
            out[x] = (r / n) | ((g / n) << 8) | ((b / n) << 16) | ((a / n) << 24);
        }
    }
    SDL_FreeSurface(s);
    return d;
}

struct Img { SDL_Texture* t = nullptr; int w = 0, h = 0; bool pending = true, keep = false; unsigned last = 0; };
struct Req { std::string key, src; int mw, mh; unsigned wanted; };
struct Done { std::string key; SDL_Surface* s; bool dropped; };
static std::map<std::string, Img> imgs;  // so a thread principal mexe aqui
static std::vector<Req> pend;
static std::vector<Done> doneQ;
static Mutex qmx;
static std::atomic<bool> loaderRun{true};

static void loaderThread(void*) {
    while (loaderRun) {
        Req r;
        bool have = false;
        mutexLock(&qmx);
        unsigned now = gFrame;
        for (size_t i = 0; i < pend.size();) {  // descarta pedidos que ja sairam da tela
            if (now > pend[i].wanted + 90) { doneQ.push_back({pend[i].key, nullptr, true}); pend.erase(pend.begin() + i); }
            else i++;
        }
        size_t best = 0;
        for (size_t i = 0; i < pend.size(); i++) if (pend[i].wanted >= pend[best].wanted) best = i;
        if (!pend.empty()) { r = pend[best]; pend.erase(pend.begin() + best); have = true; }
        mutexUnlock(&qmx);
        if (!have) { svcSleepThread(15000000LL); continue; }
        std::string path = isUrl(r.src) ? cachedFile(r.src, "") : r.src;
        SDL_Surface* s = path.empty() ? nullptr : loadScaled(path, r.mw, r.mh);
        mutexLock(&qmx);
        doneQ.push_back({r.key, s, false});
        mutexUnlock(&qmx);
    }
}
// Devolve a textura se ja estiver pronta (senao pede em segundo plano e devolve nullptr)
static SDL_Texture* getImg(const std::string& src, int mw, int mh, bool keep = false) {
    if (src.empty()) return nullptr;
    std::string key = src + "@" + std::to_string(mw) + "x" + std::to_string(mh);
    auto it = imgs.find(key);
    if (it != imgs.end()) {
        it->second.last = frameNo;
        if (it->second.pending) {
            mutexLock(&qmx);
            for (auto& r : pend) if (r.key == key) { r.wanted = gFrame; break; }
            mutexUnlock(&qmx);
        }
        return it->second.t;
    }
    Img im;
    im.keep = keep;
    im.last = frameNo;
    imgs[key] = im;
    mutexLock(&qmx);
    pend.push_back({key, src, mw, mh, gFrame});
    mutexUnlock(&qmx);
    return nullptr;
}
static void dropImgs(const std::string& src) {  // libera as texturas de uma origem (ex.: fundo trocado)
    std::string pre = src + "@";
    for (auto it = imgs.begin(); it != imgs.end();) {
        if (it->first.rfind(pre, 0) == 0 && !it->second.pending) {
            if (it->second.t) SDL_DestroyTexture(it->second.t);
            it = imgs.erase(it);
        } else ++it;
    }
}
// Passa o que a thread terminou para texturas (poucas por quadro). Retorna true se algo mudou.
static bool pumpImages() {
    std::vector<Done> ready;
    mutexLock(&qmx);
    size_t take = std::min<size_t>(doneQ.size(), 3);
    ready.assign(doneQ.begin(), doneQ.begin() + take);
    doneQ.erase(doneQ.begin(), doneQ.begin() + take);
    mutexUnlock(&qmx);
    bool changed = false;
    for (auto& d : ready) {
        auto it = imgs.find(d.key);
        if (d.dropped) { if (it != imgs.end()) imgs.erase(it); if (d.s) SDL_FreeSurface(d.s); continue; }
        if (it != imgs.end()) {
            it->second.pending = false;
            if (d.s) { it->second.t = SDL_CreateTextureFromSurface(ren, d.s); it->second.w = d.s->w; it->second.h = d.s->h; }
            changed = true;
        }
        if (d.s) SDL_FreeSurface(d.s);
    }
    mutexLock(&qmx);
    if (!doneQ.empty()) changed = true;  // ainda ha o que processar no proximo quadro
    mutexUnlock(&qmx);
    while (imgs.size() > MAX_IMGS) {  // libera a imagem usada ha mais tempo
        auto old = imgs.end();
        for (auto it = imgs.begin(); it != imgs.end(); ++it)
            if (!it->second.pending && !it->second.keep && (old == imgs.end() || it->second.last < old->second.last)) old = it;
        if (old == imgs.end()) break;
        if (old->second.t) SDL_DestroyTexture(old->second.t);
        imgs.erase(old);
    }
    return changed;
}

// ---------- desenho ----------
static void rect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(ren, &r);
}
struct TT { SDL_Texture* t; int w, h; unsigned last; };
static std::map<std::string, TT> tcache;  // textos ja desenhados (evita refazer toda hora)
static void evictText() {
    for (auto it = tcache.begin(); it != tcache.end();) {
        if (it->second.last + 20 < frameNo) { SDL_DestroyTexture(it->second.t); it = tcache.erase(it); }
        else ++it;
    }
    if (tcache.size() > 500) {
        for (auto& kv : tcache) SDL_DestroyTexture(kv.second.t);
        tcache.clear();
    }
}
static void text(TTF_Font* f, const std::string& s, int x, int y, SDL_Color c, int wrap = 0, int align = 0) {  // align: 0 esq, 1 centro, 2 dir
    if (s.empty()) return;
    char kb[48];
    snprintf(kb, sizeof kb, "%p|%02x%02x%02x|%d|", (void*)f, c.r, c.g, c.b, wrap);
    std::string key = std::string(kb) + s;
    auto it = tcache.find(key);
    if (it == tcache.end()) {
        SDL_Surface* sf = wrap ? TTF_RenderUTF8_Blended_Wrapped(f, s.c_str(), c, wrap) : TTF_RenderUTF8_Blended(f, s.c_str(), c);
        if (!sf) return;
        TT tt = {SDL_CreateTextureFromSurface(ren, sf), sf->w, sf->h, frameNo};
        SDL_FreeSurface(sf);
        if (tcache.size() > 400) evictText();
        it = tcache.emplace(key, tt).first;
    }
    it->second.last = frameNo;
    SDL_Rect d = {align == 1 ? x - it->second.w / 2 : align == 2 ? x - it->second.w : x, y, it->second.w, it->second.h};
    SDL_RenderCopy(ren, it->second.t, nullptr, &d);
}
static int textW(TTF_Font* f, const std::string& s) {
    int w = 0, h = 0;
    TTF_SizeUTF8(f, s.c_str(), &w, &h);
    return w;
}
static std::map<std::string, std::string> fitCache;
static std::string fit(TTF_Font* f, const std::string& s0, int maxw) {  // corta texto com "..." (seguro p/ UTF-8)
    char kb[40];
    snprintf(kb, sizeof kb, "%p|%d|", (void*)f, maxw);
    std::string key = std::string(kb) + s0;
    auto it = fitCache.find(key);
    if (it != fitCache.end()) return it->second;
    if (fitCache.size() > 1500) fitCache.clear();
    std::string s = s0, res = s0;
    if (textW(f, s) > maxw) {
        res = "";
        while (!s.empty()) {
            while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
            if (!s.empty()) s.pop_back();
            if (textW(f, s + "...") <= maxw) { res = s + "..."; break; }
        }
    }
    fitCache[key] = res;
    return res;
}
static void drawTex(SDL_Texture* t, int x, int y, int w, int h) {  // mantem a proporcao (contain)
    if (!t) return;
    int tw = 0, th = 0;
    SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    float sc = std::min((float)w / tw, (float)h / th);
    int dw = (int)(tw * sc), dh = (int)(th * sc);
    SDL_Rect d = {x + (w - dw) / 2, y + (h - dh) / 2, dw, dh};
    SDL_RenderCopy(ren, t, nullptr, &d);
}
static void img(const std::string& src, int x, int y, int w, int h, int mw = 300, int mh = 170) {
    rect(x, y, w, h, T->ph);
    drawTex(getImg(src, mw, mh), x, y, w, h);
}
// Fundo: imagem escolhida pelo usuario (Configuracoes) cobrindo a tela, escurecida para o texto ficar legivel
static void begin() {
    SDL_SetRenderDrawColor(ren, T->bg.r, T->bg.g, T->bg.b, 255);
    SDL_RenderClear(ren);
    std::string p = cfgStr("fundo_img");
    if (p.empty()) return;
    SDL_Texture* t = getImg(p, W, H, true);
    if (!t) return;
    int tw = 0, th = 0;
    SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    float sc = std::max((float)W / tw, (float)H / th);  // cover
    int dw = (int)(tw * sc), dh = (int)(th * sc);
    SDL_Rect d = {(W - dw) / 2, (H - dh) / 2, dw, dh};
    SDL_RenderCopy(ren, t, nullptr, &d);
    rect(0, 0, W, H, {T->bg.r, T->bg.g, T->bg.b, 165});
}

// ---------- logo ----------
static SDL_Texture* logoTex = nullptr;
static int logoTW = 0, logoTH = 0;
static void loadLogo() {  // sdmc:/switch/romstore/logo.png (troca facil) ou a logo embutida no app (romfs)
    const char* c[] = {"sdmc:/switch/romstore/logo.png", "romfs:/logo.png"};
    for (const char* p : c) {
        if (!exists(p)) continue;
        SDL_Surface* s = loadScaled(p, 700, 200);
        if (!s) continue;
        logoTex = SDL_CreateTextureFromSurface(ren, s);
        logoTW = s->w; logoTH = s->h;
        SDL_FreeSurface(s);
        if (logoTex) return;
    }
}
static int logoWidth(int h) { return logoTex && logoTH > 0 ? logoTW * h / logoTH : 230; }
static void drawLogo(int x, int y, int h) {
    if (logoTex) { SDL_Rect d = {x, y, logoWidth(h), h}; SDL_RenderCopy(ren, logoTex, nullptr, &d); }
    else text(fL, "ROM STORE", x, y + (h - 52) / 2, ONACC);
}
// Barra no topo na cor do tema, com a logo
static void drawHeader(const std::string& title, const std::string& right = "") {
    rect(0, 0, W, HDR, T->accent);
    drawLogo(24, 10, 56);
    int x = 24 + logoWidth(56) + 36;
    if (!title.empty()) text(fM, fit(fM, title, 560), x, 20, ONACC);
    if (!right.empty()) text(fM, fit(fM, right, 380), W - 24, 20, ONACC, 0, 2);
}

// ---------- catalogo ----------
static std::string catalogUrl() {
    std::string u = cfgStr("catalogo");
    return u.empty() ? DEFAULT_URL : u;
}
static bool loadCatalog(const std::string& url) {
    catalogBase = url.substr(0, url.rfind('/') + 1);
    std::string data;
    if (!fetch(url, data)) { loadError = "Não foi possível baixar o catálogo.\nConfira a internet e o endereço no config.ini"; return false; }
    json j = json::parse(data, nullptr, false);
    std::string().swap(data);  // libera o texto do catalogo
    if (j.is_discarded() || !j.is_object()) { loadError = "Catálogo inválido (erro no JSON)"; return false; }
    auto S = [](const json& o, const char* k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string(); };
    if (j.contains("categorias") && j["categorias"].is_array() && !j["categorias"].empty()) {
        consoles.clear();
        for (auto& c : j["categorias"]) if (c.is_string()) consoles.push_back({c.get<std::string>(), "", 0});
    }
    if (j.contains("imagens") && j["imagens"].is_object())  // "imagens": {"DS": "imagens/ds.png"}  (imagem do card do console)
        for (auto it = j["imagens"].begin(); it != j["imagens"].end(); ++it)
            if (it.value().is_string())
                for (auto& c : consoles) if (lower(c.nome) == lower(it.key())) c.imagem = resolveUrl(it.value().get<std::string>());
    if (j.contains("audio") && j["audio"].is_object()) {  // "audio": {"abertura": "...", "clique": "...", "fundo": "...", "versao": "1"}
        const json& a = j["audio"];
        audAbertura = S(a, "abertura"); audClique = S(a, "clique"); audFundo = S(a, "fundo"); audVersao = S(a, "versao");
    }
    if (j.contains("jogos") && j["jogos"].is_array()) {
        games.reserve(j["jogos"].size());
        for (auto& o : j["jogos"]) {
            if (!o.is_object()) continue;
            Game g;
            g.id = S(o, "id"); g.titulo = S(o, "titulo"); g.sistema = S(o, "sistema"); g.descricao = S(o, "descricao");
            g.capa = resolveUrl(S(o, "capa")); g.url = S(o, "url"); g.arquivo = S(o, "arquivo"); g.tamanho = S(o, "tamanho");
            g.letter = letterOf(g.titulo);
            g.tl = lower(g.titulo); g.sl = lower(g.sistema);
            if (o.contains("fotos") && o["fotos"].is_array())
                for (auto& f : o["fotos"]) if (f.is_string()) g.fotos.push_back(resolveUrl(f.get<std::string>()));
            games.push_back(std::move(g));
        }
    }
    for (auto& c : consoles) {
        std::string l = lower(c.nome);
        for (auto& g : games) if (g.sl == l) c.count++;
    }
    return true;
}
// Monta a lista de letras do console aberto (so as que existem) e os jogos da letra escolhida
static void refilter() {
    view.clear();
    letters.clear();
    std::string q = lower(query), c = lower(consoles[ci].nome);
    std::vector<int> base;
    std::set<char> L;
    for (size_t i = 0; i < games.size(); i++) {
        if (games[i].sl != c) continue;
        if (!q.empty() && games[i].tl.find(q) == std::string::npos) continue;
        base.push_back((int)i);
        L.insert(games[i].letter);
    }
    std::sort(base.begin(), base.end(), [](int a, int b) { return games[a].tl < games[b].tl; });
    letters.push_back("Todos");
    for (char ch : L) if (ch != '#') letters.push_back(std::string(1, ch));
    if (L.count('#')) letters.push_back("#");
    if (std::find(letters.begin(), letters.end(), curLetter) == letters.end()) curLetter = "Todos";
    for (int i : base)
        if (curLetter == "Todos" || std::string(1, games[i].letter) == curLetter) view.push_back(i);
}
static int letterIndex() {
    auto it = std::find(letters.begin(), letters.end(), curLetter);
    return it == letters.end() ? 0 : (int)(it - letters.begin());
}

// ---------- teclado e seletor de pasta/imagem ----------
static bool askText(std::string& out, const char* guide) {  // teclado nativo do Switch
    SwkbdConfig k;
    char buf[64] = {0};
    if (R_FAILED(swkbdCreate(&k, 0))) return false;
    swkbdConfigMakePresetDefault(&k);
    swkbdConfigSetGuideText(&k, guide);
    Result rc = swkbdShow(&k, buf, sizeof buf);
    swkbdClose(&k);
    if (R_FAILED(rc)) return false;
    out = buf;
    return true;
}
static std::string parentDir(const std::string& p) {  // "" = ja esta na raiz
    std::string s = p;
    while (s.size() > 6 && s.back() == '/') s.pop_back();
    if (s == "sdmc:/" || s == "sdmc:") return "";
    size_t k = s.rfind('/');
    return k == std::string::npos || k <= 5 ? "sdmc:/" : s.substr(0, k);
}
struct Ent { std::string name; bool dir; };
static bool isImageName(const std::string& n) {
    std::string l = lower(n);
    for (const char* e : {".jpg", ".jpeg", ".png"})
        if (l.size() > strlen(e) && l.compare(l.size() - strlen(e), strlen(e), e) == 0) return true;
    return false;
}
static bool listEntries(const std::string& path, bool files, std::vector<Ent>& out) {
    out.clear();
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (e->d_type == DT_DIR) out.push_back({n, true});
        else if (files && isImageName(n)) out.push_back({n, false});
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const Ent& a, const Ent& b) { return a.dir != b.dir ? a.dir : lower(a.name) < lower(b.name); });
    return true;
}
// Navegador do cartao SD.
//  files=false: escolhe uma PASTA (X = usar a pasta atual, Y = nova pasta). Retorna a pasta ou "" se cancelar.
//  files=true : escolhe uma IMAGEM jpg/png (A no arquivo, Y = tirar a imagem de fundo). Retorna o arquivo, "-" (remover) ou "".
static std::string pickPath(std::string cur, const std::string& title, bool files) {
    std::vector<Ent> ents;
    while (!listEntries(cur, files, ents)) {
        cur = parentDir(cur);
        if (cur.empty()) { cur = "sdmc:/"; listEntries(cur, files, ents); break; }
    }
    int sel = 0;
    while (appletMainLoop()) {
        gFrame = ++frameNo;
        pumpImages();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type != SDL_JOYBUTTONDOWN) continue;
            int b = e.jbutton.button;
            if (b >= 16 && b <= 19) b -= 4;
            playClick();
            int n = (int)ents.size();
            if (b == 13 && sel > 0) sel--;
            else if (b == 15 && sel < n - 1) sel++;
            else if (b == 6) sel = std::max(0, sel - 9);
            else if (b == 7) sel = std::min(std::max(0, n - 1), sel + 9);
            else if (b == 0 && n > 0) {
                if (ents[sel].dir) { cur = joinPath(cur, ents[sel].name); listEntries(cur, files, ents); sel = 0; }
                else return joinPath(cur, ents[sel].name);
            }
            else if (b == 1) {
                std::string up = parentDir(cur);
                if (up.empty()) return "";
                cur = up; listEntries(cur, files, ents); sel = 0;
            }
            else if (b == 2 && !files) return cur;
            else if (b == 3) {
                if (files) return "-";
                std::string name;
                if (askText(name, "Nome da nova pasta") && safeName(name)) {
                    mkdir(joinPath(cur, name).c_str(), 0777);
                    listEntries(cur, files, ents);
                    for (size_t i = 0; i < ents.size(); i++) if (ents[i].dir && ents[i].name == name) sel = (int)i;
                }
            }
        }
        begin();
        drawHeader(title);
        text(fS, fit(fS, "Pasta atual: " + cur, 1230), 24, 92, T->accentL);
        int n = (int)ents.size(), rows = 9, off = std::max(0, std::min(sel - rows / 2, n - rows));
        if (n == 0) text(fS, files ? "(nenhuma imagem ou pasta aqui)" : "(sem subpastas - use X para escolher esta pasta ou Y para criar uma)", 40, 150, T->gray);
        for (int r = 0; r < rows && off + r < n; r++) {
            int y = 130 + r * 50;
            if (off + r == sel) rect(30, y - 4, 1220, 46, T->accent);
            text(fM, fit(fM, (ents[off + r].dir ? "[ ] " : "") + ents[off + r].name, 1150), 50, y, off + r == sel ? ONACC : T->text);
        }
        text(fS, files ? "A: abrir/escolher     B: voltar     Y: sem imagem de fundo     L/R: pular" : "A: abrir     B: voltar     X: usar esta pasta     Y: nova pasta     L/R: pular", 40, 684, T->gray);
        SDL_RenderPresent(ren);
    }
    return "";
}

// ---------- mini game Snake (durante o download) ----------
struct Snake {
    static const int CW = 32, CH = 18, CS = 20;  // campo de 640x360
    std::deque<std::pair<int, int>> body;
    int dx = 1, dy = 0, ndx = 1, ndy = 0, fx = 0, fy = 0, score = 0;
    bool dead = false;
    Uint32 last = 0;
    void place() {
        for (;;) {
            fx = rand() % CW; fy = rand() % CH;
            bool ok = true;
            for (auto& p : body) if (p.first == fx && p.second == fy) ok = false;
            if (ok) return;
        }
    }
    void reset() {
        body.clear();
        for (int i = 0; i < 3; i++) body.push_back({10 - i, CH / 2});
        dx = ndx = 1; dy = ndy = 0; score = 0; dead = false;
        place();
        last = SDL_GetTicks();
    }
    void turn(int x, int y) { if (!(x == -dx && y == -dy)) { ndx = x; ndy = y; } }
    void step() {
        if (dead) return;
        Uint32 now = SDL_GetTicks();
        Uint32 iv = (Uint32)std::max(60, 130 - score * 3);
        if (now - last < iv) return;
        last = now;
        dx = ndx; dy = ndy;
        int hx = (body.front().first + dx + CW) % CW, hy = (body.front().second + dy + CH) % CH;  // atravessa as bordas
        for (size_t i = 0; i + 1 < body.size(); i++)
            if (body[i].first == hx && body[i].second == hy) { dead = true; return; }
        body.push_front({hx, hy});
        if (hx == fx && hy == fy) { score++; place(); }
        else body.pop_back();
    }
    void draw(int x0, int y0) const {
        rect(x0 - 4, y0 - 4, CW * CS + 8, CH * CS + 8, T->accent);
        rect(x0, y0, CW * CS, CH * CS, {12, 12, 16, 255});
        rect(x0 + fx * CS + 3, y0 + fy * CS + 3, CS - 6, CS - 6, {235, 60, 60, 255});
        for (size_t i = 0; i < body.size(); i++)
            rect(x0 + body[i].first * CS + 1, y0 + body[i].second * CS + 1, CS - 2, CS - 2, i == 0 ? T->sel : T->accentL);
        if (dead) {
            rect(x0, y0 + CH * CS / 2 - 50, CW * CS, 100, {0, 0, 0, 200});
            text(fL, "Game Over", x0 + CW * CS / 2, y0 + CH * CS / 2 - 44, ONACC, 0, 1);
            text(fS, "A: jogar de novo", x0 + CW * CS / 2, y0 + CH * CS / 2 + 14, T->accentL, 0, 1);
        }
    }
};

// ---------- download (em outra thread, para a tela e o jogo nao travarem) ----------
struct Job {
    std::string url;
    FILE* f = nullptr;
    std::atomic<long long> now{0}, tot{0};
    std::atomic<bool> cancel{false}, done{false};
    CURLcode rc = CURLE_OK;
};
static int jobProgress(void* u, curl_off_t tot, curl_off_t now, curl_off_t, curl_off_t) {
    Job* j = (Job*)u;
    j->tot = tot;
    j->now = now;
    return j->cancel ? 1 : 0;
}
static void jobThread(void* u) {
    Job* j = (Job*)u;
    CURL* c = curl_easy_init();
    if (!c) { j->rc = CURLE_FAILED_INIT; j->done = true; return; }
    setup(c, j->url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wFile);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, j->f);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 262144L);  // blocos grandes = menos chamadas e menos escrita no SD
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, jobProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, j);
    j->rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    j->done = true;
}
static void drawDownload(const Game& g, Job& job, Snake& sn, bool useSnake) {
    begin();
    text(fL, "Baixando...", W / 2, 14, T->text, 0, 1);
    text(fM, fit(fM, g.titulo, 1000), W / 2, 66, T->gray, 0, 1);
    if (useSnake) {
        text(fS, "Snake   |   Pontos: " + std::to_string(sn.score) + "   |   X: esconder", W / 2, 108, T->accentL, 0, 1);
        sn.draw((W - Snake::CW * Snake::CS) / 2, 138);
    } else {
        text(fM, "Quer jogar Snake enquanto baixa?", W / 2, 270, T->text, 0, 1);
        text(fS, "Pressione X", W / 2, 320, T->accentL, 0, 1);
    }
    long long tot = job.tot, now = job.now;
    rect(240, 556, 800, 28, T->ph);
    if (tot > 0) rect(240, 556, (int)(800.0 * now / tot), 28, T->accent);
    else rect(240 + (int)((SDL_GetTicks() / 5) % 700), 556, 100, 28, T->accent);  // tamanho desconhecido
    char b[64];
    snprintf(b, sizeof b, "%.1f MB / %.1f MB", now / 1048576.0, tot / 1048576.0);
    text(fS, b, W / 2, 592, T->text, 0, 1);
    text(fS, "B: cancelar", W / 2, 684, T->gray, 0, 1);
    SDL_RenderPresent(ren);
}
static std::string downloadRom(const Game& g, const std::string& dir) {  // retorna "" se deu certo, ou a mensagem de erro
    if (!safeName(g.arquivo) || !safeName(g.sistema) || g.url.empty()) return "Dados do jogo inválidos no catálogo";
    mkdirs(dir);
    std::string dst = joinPath(dir, g.arquivo), tmp = dst + ".part";
    Job job;
    job.url = g.url;
    job.f = fopen(tmp.c_str(), "wb");
    if (!job.f) return "Não foi possível criar o arquivo no cartão SD";
    setvbuf(job.f, nullptr, _IOFBF, 262144);
    Thread th;
    if (R_FAILED(threadCreate(&th, jobThread, &job, NULL, 0x100000, 0x2C, -2)) || R_FAILED(threadStart(&th))) {
        fclose(job.f);
        remove(tmp.c_str());
        return "Não foi possível iniciar o download";
    }
    Snake sn;
    sn.reset();
    while (!job.done) {
        gFrame = ++frameNo;
        if (!appletMainLoop()) job.cancel = true;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type != SDL_JOYBUTTONDOWN) continue;
            int b = e.jbutton.button;
            if (b >= 16 && b <= 19) b -= 4;
            if (b == 1) job.cancel = true;
            else if (b == 2) cfgSet("snake", cfgInt("snake", 1) ? "0" : "1");
            else if (cfgInt("snake", 1)) {
                if (b == 12) sn.turn(-1, 0);
                else if (b == 14) sn.turn(1, 0);
                else if (b == 13) sn.turn(0, -1);
                else if (b == 15) sn.turn(0, 1);
                else if (b == 0 && sn.dead) sn.reset();
            }
        }
        bool useSnake = cfgInt("snake", 1);
        if (useSnake) sn.step();
        drawDownload(g, job, sn, useSnake);
        SDL_Delay(16);  // ~30 quadros por segundo: suficiente para o jogo e esquenta menos
    }
    threadWaitForExit(&th);
    threadClose(&th);
    fclose(job.f);
    if (job.rc == CURLE_OK) { remove(dst.c_str()); rename(tmp.c_str(), dst.c_str()); return ""; }
    remove(tmp.c_str());
    return job.rc == CURLE_ABORTED_BY_CALLBACK ? "Download cancelado" : std::string("Falha no download: ") + curl_easy_strerror(job.rc);
}
static std::vector<std::string> gallery(const Game& g) {
    std::vector<std::string> v;
    if (!g.capa.empty()) v.push_back(g.capa);
    for (auto& f : g.fotos) v.push_back(f);
    return v;
}

// ---------- loading (tela estilo eShop: barra de ponta a ponta + logo no canto) ----------
static std::atomic<bool> bootDone{false};
static void bootThread(void*) {  // baixa o catalogo e os sons enquanto a tela de loading roda
    loadCatalog(catalogUrl());
    prefetchAudio();
    bootDone = true;
}
static bool splash() {  // retorna false se o usuario saiu
    Uint32 t0 = SDL_GetTicks();
    while (appletMainLoop()) {
        gFrame = ++frameNo;
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_JOYBUTTONDOWN && e.jbutton.button == 10) return false;
        Uint32 el = SDL_GetTicks() - t0;
        double p = std::min(1.0, el / (double)SPLASH_MS);
        p = 1.0 - (1.0 - p) * (1.0 - p);  // desacelera no final
        if (!bootDone) p = std::min(p, 0.94);
        SDL_SetRenderDrawColor(ren, T->accent.r, T->accent.g, T->accent.b, 255);
        SDL_RenderClear(ren);
        const int by = H - 150, bh = 14;
        rect(0, by, W, bh, {0, 0, 0, 70});                                  // trilho de ponta a ponta
        rect(0, by, (int)(W * p), bh, ONACC);                                // progresso da esquerda para a direita
        SDL_Rect clip = {0, by, W, bh};
        SDL_RenderSetClipRect(ren, &clip);
        int sx = (int)((el / 2) % (W + 400)) - 400;                          // brilho que corre de um lado ao outro
        rect(sx, by, 400, bh, {255, 255, 255, 90});
        SDL_RenderSetClipRect(ren, nullptr);
        text(fS, bootDone || el < 1500 ? "Carregando..." : "Conectando ao catálogo...", 40, by - 40, ONACC);
        drawLogo(W - 40 - logoWidth(110), H - 40 - 110, 110);               // logo embaixo, lado direito
        SDL_RenderPresent(ren);
        if (el >= SPLASH_MS && bootDone) return true;
    }
    return false;
}

// ---------- telas ----------
enum Scr { HOME, LIST, DETAIL, SETTINGS };

int main(int, char**) {
    socketInitializeDefault();
    plInitialize(PlServiceType_User);
    romfsInit();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO);
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
    TTF_Init();
    for (int i = 0; i < SDL_NumJoysticks(); i++) SDL_JoystickOpen(i);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Window* win = SDL_CreateWindow("ROM Store", 0, 0, W, H, SDL_WINDOW_SHOWN);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    PlFontData fd;
    plGetSharedFontByType(&fd, PlSharedFontType_Standard);
    fS = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 22);
    fM = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 28);
    fL = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 42);
    mutexInit(&qmx);
    srand((unsigned)time(nullptr));

    loadConfig();
    setTheme(cfgInt("tema", 0));
    loadLogo();

    Thread loadTh, bootTh;
    bool haveLoader = R_SUCCEEDED(threadCreate(&loadTh, loaderThread, nullptr, NULL, 0x100000, 0x30, -2)) && R_SUCCEEDED(threadStart(&loadTh));
    bool haveBoot = R_SUCCEEDED(threadCreate(&bootTh, bootThread, nullptr, NULL, 0x100000, 0x2C, -2)) && R_SUCCEEDED(threadStart(&bootTh));
    if (!haveBoot) { bootThread(nullptr); }  // sem thread: carrega aqui mesmo
    bool run = splash();
    if (haveBoot) { threadWaitForExit(&bootTh); threadClose(&bootTh); }
    if (run) loadAudio();

    Scr scr = HOME, prev = HOME;
    bool alphaOpen = false;
    float alphaAnim = 0.f;
    int sel = 0, ssel = 0, thumb = 0;
    std::string msg;
    Uint32 msgUntil = 0, lastDraw = 0;
    if (!consoles.empty()) refilter();

    auto setLetter = [&](int i) {
        if (letters.empty()) return;
        i = std::max(0, std::min((int)letters.size() - 1, i));
        curLetter = letters[i];
        sel = 0;
        refilter();
    };
    const int NFIXED = 5;  // linhas fixas das configuracoes; depois vem uma pasta por console
    bool dirty = true;

    while (run && appletMainLoop()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            dirty = true;
            if (e.type == SDL_QUIT) run = false;
            if (e.type != SDL_JOYBUTTONDOWN) continue;
            int b = e.jbutton.button;  // 0 A, 1 B, 2 X, 3 Y, 6 L, 7 R, 8 ZL, 10 +, 11 -, 12 esq, 13 cima, 14 dir, 15 baixo, 16-19 analogico esq
            if (b >= 16 && b <= 19) b -= 4;
            if (b == 10) { run = false; continue; }
            playClick();
            int n = (int)view.size(), nc = (int)consoles.size();
            if (scr == HOME) {
                if (b == 12 || b == 6) ci = std::max(0, ci - 1);
                else if (b == 14 || b == 7) ci = std::min(nc - 1, ci + 1);
                else if (b == 3 || b == 11) { prev = HOME; scr = SETTINGS; ssel = 0; }
                else if (b == 0 && nc > 0) { curLetter = "Todos"; query.clear(); sel = 0; alphaOpen = false; refilter(); scr = LIST; }
            } else if (scr == LIST && alphaOpen) {  // alfabeto suspenso aberto
                int li = letterIndex(), nl = (int)letters.size();
                if (b == 12 || b == 6) setLetter(li - 1);
                else if (b == 14 || b == 7) setLetter(li + 1);
                else if (b == 13) setLetter(li - ALPHA_COLS >= 0 ? li - ALPHA_COLS : li);
                else if (b == 15) setLetter(li + ALPHA_COLS < nl ? li + ALPHA_COLS : li);
                else if (b == 0 || b == 1 || b == 8) alphaOpen = false;
            } else if (scr == LIST) {
                if (b == 1) scr = HOME;
                else if (b == 3 || b == 11) { prev = LIST; scr = SETTINGS; ssel = 0; }
                else if (b == 2) { std::string t; if (askText(t, "Buscar jogo")) { query = t; sel = 0; refilter(); } }
                else if (b == 8) alphaOpen = true;
                else if (b == 6) setLetter(letterIndex() - 1);
                else if (b == 7) setLetter(letterIndex() + 1);
                else if (b == 12) { if (sel % COLS > 0) sel--; }
                else if (b == 14) { if (sel % COLS < COLS - 1 && sel + 1 < n) sel++; }
                else if (b == 13) { if (sel - COLS >= 0) sel -= COLS; else alphaOpen = true; }
                else if (b == 15) { if (sel + COLS < n) sel += COLS; }
                else if (b == 0 && n > 0) { scr = DETAIL; thumb = 0; }
            } else if (scr == DETAIL) {
                const Game& g = games[view[sel]];
                int cnt = std::max(1, (int)gallery(g).size());
                std::string dir = romDir(g.sistema);
                if (b == 1) scr = LIST;
                else if (b == 12 || b == 6) thumb = (thumb + cnt - 1) % cnt;
                else if (b == 14 || b == 7) thumb = (thumb + 1) % cnt;
                else if (b == 2) {  // X: escolher / mudar a pasta de download deste console
                    std::string p = pickPath(dir.empty() ? defaultDir(g.sistema) : dir, "Pasta das ROMs de " + g.sistema, false);
                    if (!p.empty()) { cfgSet("rom_" + lower(g.sistema), p); msg = "Pasta salva: " + p; msgUntil = SDL_GetTicks() + 5000; }
                }
                else if (b == 0) {
                    if (dir.empty()) {  // primeira vez neste console: escolhe a pasta
                        dir = pickPath(defaultDir(g.sistema), "Pasta das ROMs de " + g.sistema, false);
                        if (dir.empty()) { msg = "Escolha uma pasta para baixar"; msgUntil = SDL_GetTicks() + 5000; }
                        else cfgSet("rom_" + lower(g.sistema), dir);
                    }
                    if (!dir.empty()) {
                        std::string err = downloadRom(g, dir);
                        msg = err.empty() ? "Salvo em " + destPath(g) : err;
                        msgUntil = SDL_GetTicks() + 7000;
                    }
                }
            } else {  // SETTINGS
                int items = NFIXED + nc;
                if (b == 13 && ssel > 0) ssel--;
                else if (b == 15 && ssel < items - 1) ssel++;
                else if (b == 1 || b == 3 || b == 11) scr = prev;
                else if (b == 0 || b == 12 || b == 14) {
                    int d = b == 12 ? -1 : 1;
                    if (ssel == 0) { int t = ((int)(T - THEMES) + d + NTHEMES) % NTHEMES; setTheme(t); cfgSet("tema", std::to_string(t)); }
                    else if (ssel == 1) {  // escolher a imagem de fundo
                        if (b == 0) {
                            mkdirs(BG_DIR);
                            std::string old = cfgStr("fundo_img"), p = pickPath(BG_DIR, "Escolha a imagem de fundo (jpg/png)", true);
                            if (!p.empty()) {
                                if (!old.empty()) dropImgs(old);
                                cfgSet("fundo_img", p == "-" ? "" : p);
                            }
                        }
                    }
                    else if (ssel == 2) cfgSet("sfx", cfgInt("sfx", 1) ? "0" : "1");
                    else if (ssel == 3) { cfgSet("musica", cfgInt("musica", 1) ? "0" : "1"); applyMusic(); }
                    else if (ssel == 4) cfgSet("snake", cfgInt("snake", 1) ? "0" : "1");
                    else if (b == 0) {
                        const std::string& sys = consoles[ssel - NFIXED].nome;
                        std::string cur = romDir(sys), p = pickPath(cur.empty() ? defaultDir(sys) : cur, "Pasta das ROMs de " + sys, false);
                        if (!p.empty()) cfgSet("rom_" + lower(sys), p);
                    }
                }
            }
        }
        if (pumpImages()) dirty = true;
        float target = alphaOpen ? 1.f : 0.f;  // animacao do alfabeto descendo/subindo
        if (alphaAnim != target) {
            alphaAnim += alphaAnim < target ? 0.18f : -0.18f;
            if (alphaAnim < 0.f) alphaAnim = 0.f;
            if (alphaAnim > 1.f) alphaAnim = 1.f;
            dirty = true;
        }
        if (msgUntil && SDL_GetTicks() >= msgUntil) { msgUntil = 0; dirty = true; }
        if (SDL_GetTicks() - lastDraw > 500) dirty = true;
        if (!dirty) { SDL_Delay(25); continue; }  // nada mudou: nao redesenha (menos CPU/GPU e menos calor)
        dirty = false;
        lastDraw = SDL_GetTicks();
        gFrame = ++frameNo;

        int n = (int)view.size(), nc = (int)consoles.size();
        begin();

        if (scr == HOME) {
            drawHeader("Consoles");
            for (int i = 0; i < nc; i++) {  // carrossel de consoles: o selecionado fica no centro
                int x = W / 2 - 170 + (i - ci) * 370, y = 120;
                if (x > W || x + 340 < 0) continue;
                if (i == ci) rect(x - 6, y - 6, 352, 412, T->sel);
                rect(x, y, 340, 400, T->card);
                rect(x + 10, y + 10, 320, 230, T->ph);
                drawTex(getImg(consoles[i].imagem, 640, 360, true), x + 10, y + 10, 320, 230);
                text(fL, fit(fL, consoles[i].nome, 320), x + 170, y + 256, T->ink, 0, 1);
                text(fS, std::to_string(consoles[i].count) + " jogos", x + 170, y + 330, {110, 110, 120, 255}, 0, 1);
                if (i != ci) rect(x, y, 340, 400, {T->bg.r, T->bg.g, T->bg.b, 120});
            }
            if (ci > 0) text(fL, "<", 14, 290, T->text);
            if (ci < nc - 1) text(fL, ">", W - 14, 290, T->text, 0, 2);
            if (games.empty() && !loadError.empty()) text(fS, fit(fS, loadError, 1200), 40, 620, T->gray);
            text(fS, "Esq/Dir: console     A: abrir     Y: configurações     +: sair", 40, 684, T->gray);
        } else if (scr == LIST) {
            drawHeader(consoles[ci].nome + (query.empty() ? "" : "  -  busca: " + query), "Letra: " + curLetter);
            if (n == 0) text(fM, "Nenhum jogo encontrado", 40, 130, T->gray, 1100);
            int page = sel / PER;
            for (int k = 0; k < PER; k++) {
                int i = page * PER + k;
                if (i >= n) break;
                const Game& g = games[view[i]];
                int gx = 40 + (k % COLS) * 305, gy = 100 + (k / COLS) * 240;
                if (i == sel && !alphaOpen) rect(gx - 5, gy - 5, 300, 218, T->sel);
                rect(gx, gy, 290, 208, T->card);
                img(g.capa, gx, gy, 290, 160);
                text(fS, fit(fS, g.titulo, 270), gx + 10, gy + 166, T->ink);
            }
            int pages = std::max(1, (n + PER - 1) / PER);
            for (int p = 0; p < pages && p < 20; p++) rect(W / 2 - pages * 7 + p * 14, 610, 8, 8, p == page ? T->text : T->gray);
            if (n > 0) text(fS, std::to_string(sel + 1) + " / " + std::to_string(n), W - 40, 640, T->gray, 0, 2);
            text(fS, "ZL: alfabeto     L/R: letra     A: abrir     X: buscar     Y: configurações     B: voltar", 40, 684, T->gray);
            if (alphaAnim > 0.f) {  // alfabeto suspenso: desce da barra, na cor do tema
                int nl = (int)letters.size(), rows = (nl + ALPHA_COLS - 1) / ALPHA_COLS, ph = rows * 62 + 24;
                SDL_Rect clip = {0, HDR, W, ph + 12};
                SDL_RenderSetClipRect(ren, &clip);
                int y0 = HDR - (int)((1.f - alphaAnim) * ph);
                rect(0, y0, W, ph, T->accent);
                rect(0, y0 + ph - 4, W, 4, T->accentL);
                int cur = letterIndex();
                for (int i = 0; i < nl; i++) {
                    int cx = 52 + (i % ALPHA_COLS) * 84, cy = y0 + 12 + (i / ALPHA_COLS) * 62;
                    if (i == cur) rect(cx, cy, 80, 54, T->sel);
                    TTF_Font* f = letters[i].size() > 1 ? fS : fM;
                    text(f, letters[i], cx + 40, cy + (letters[i].size() > 1 ? 14 : 10), i == cur ? T->ink : ONACC, 0, 1);
                }
                SDL_RenderSetClipRect(ren, nullptr);
            }
        } else if (scr == DETAIL && n > 0) {
            const Game& g = games[view[sel]];
            std::vector<std::string> gl = gallery(g);
            if (!gl.empty()) { thumb = std::min(thumb, (int)gl.size() - 1); img(gl[thumb], 40, 50, 720, 405, 720, 405); }
            else rect(40, 50, 720, 405, T->ph);
            for (size_t i = 0; i < gl.size() && i < 5; i++) {
                int tx = 40 + (int)i * 144;
                if ((int)i == thumb) rect(tx - 4, 481, 138, 82, T->sel);
                img(gl[i], tx, 485, 130, 74);
            }
            text(fS, g.sistema + (g.tamanho.empty() ? "" : "   |   " + g.tamanho), 800, 50, T->gray);
            text(fM, g.titulo, 800, 85, T->text, 440);
            SDL_Rect clip = {800, 190, 440, 360};
            SDL_RenderSetClipRect(ren, &clip);
            text(fS, g.descricao, 800, 190, T->text, 440);
            SDL_RenderSetClipRect(ren, nullptr);
            bool done = exists(destPath(g));
            rect(800, 570, 440, 64, done ? GREEN : T->accent);
            text(fM, done ? "Baixado (A: baixar de novo)" : "Baixar", 1020, 582, ONACC, 0, 1);
            std::string rd = romDir(g.sistema);
            text(fS, fit(fS, "Pasta: " + (rd.empty() ? "(escolha ao baixar)" : rd) + "     X: mudar pasta", 1200), 40, 590, T->accentL);
            if (msgUntil) text(fS, fit(fS, msg, 1200), 40, 635, T->text);
            text(fS, "A: baixar     X: pasta     B: voltar     Esq/Dir: fotos", 40, 684, T->gray);
        } else if (scr == SETTINGS) {
            drawHeader("Configurações");
            int items = NFIXED + nc, rows = 9, off = std::max(0, std::min(ssel - rows / 2, items - rows));
            for (int r = 0; r < rows && off + r < items; r++) {
                int i = off + r, y = 100 + r * 58;
                std::string label, value;
                auto onoff = [](const char* k) { return std::string(cfgInt(k, 1) ? "Ligado" : "Desligado"); };
                if (i == 0) { label = "Tema"; value = std::string("<  ") + T->name + "  >"; }
                else if (i == 1) { label = "Imagem de fundo"; value = cfgStr("fundo_img").empty() ? "(nenhuma - A: escolher)" : baseName(cfgStr("fundo_img")); }
                else if (i == 2) { label = "Som ao apertar botões"; value = onoff("sfx"); }
                else if (i == 3) { label = "Música de fundo"; value = onoff("musica"); }
                else if (i == 4) { label = "Mini game Snake no download"; value = onoff("snake"); }
                else {
                    const std::string& sys = consoles[i - NFIXED].nome;
                    label = "Pasta de download - " + sys;
                    value = romDir(sys).empty() ? "(escolher)" : romDir(sys);
                }
                if (i == ssel) rect(40, y - 6, 1200, 52, T->accent);
                text(fM, label, 60, y, i == ssel ? ONACC : T->text);
                text(fS, fit(fS, value, 560), 1220, y + 4, i == ssel ? ONACC : T->accentL, 0, 2);
            }
            text(fS, "Cima/Baixo: escolher     A: abrir/alterar     Esq/Dir: mudar     B: voltar", 40, 684, T->gray);
        }
        SDL_RenderPresent(ren);
    }

    loaderRun = false;
    if (haveLoader) { threadWaitForExit(&loadTh); threadClose(&loadTh); }
    for (auto& kv : imgs) if (kv.second.t) SDL_DestroyTexture(kv.second.t);
    for (auto& kv : tcache) SDL_DestroyTexture(kv.second.t);
    if (logoTex) SDL_DestroyTexture(logoTex);
    if (audioOk) {
        Mix_HaltMusic();
        if (sClick) Mix_FreeChunk(sClick);
        if (sOpen) Mix_FreeChunk(sOpen);
        if (bgm) Mix_FreeMusic(bgm);
        Mix_CloseAudio();
        Mix_Quit();
    }
    TTF_Quit(); IMG_Quit(); SDL_Quit();
    curl_global_cleanup();
    romfsExit();
    plExit();
    socketExit();
    return 0;
}
