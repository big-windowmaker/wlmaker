/* ========================================================================= */
/**
 * @file i18n.c
 *
 * @copyright
 * Copyright (c) 2026 Big WindowMaker contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "i18n.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* == Declarations ========================================================= */

/** A translation. */
typedef struct {
    /** Text in English. */
    const char                *text_ptr;
    /** Translated text. */
    const char                *translation_ptr;
} wlmaker_i18n_entry_t;

/** Translations to Brazilian Portuguese (pt_BR). */
static const wlmaker_i18n_entry_t _wlmaker_i18n_pt_br[] = {
    { "Next Workspace", "Próxima área" },
    { "Previous Workspace", "Área anterior" },
    { "New Workspace", "Nova área de trabalho" },
    { "Destroy Last Workspace", "Destruir última área" },
    { "Move Up", "Mover para cima" },
    { "Move Down", "Mover para baixo" },
    { "Move Left", "Mover para a esquerda" },
    { "Move Right", "Mover para a direita" },
    { "Move to Left Side", "Mover para o lado esquerdo" },
    { "Move to Right Side", "Mover para o lado direito" },
    { "Move to Top", "Mover para o topo" },
    { "Move to Bottom", "Mover para a base" },
    { "Launch", "Executar" },
    { "Kill", "Terminar" },
    { "Remove", "Remover" },
    // Window Maker's menu of docked icons, as its Portuguese catalog (pt.po)
    // has them. "Dock position", "Normal", "Auto raise & lower", "Add a
    // drawer" and "Autoraise" are untranslated there, and stay so.
    { "Bring Here", "Trazer aqui" },
    { "Unhide Here", "Mostrar Aqui" },
    { "Hide", "Esconder" },
    { "Unhide", "Mostrar" },
    { "Settings...", "Configurações..." },
    { "Keep on Top", "Manter sobre outros" },
    { "Clip Options", "Opções do Clip" },
    { "Remove Icon", "Remover Ícone" },
    { "Kill Application", "Terminar Aplicação" },
    { " will be forcibly closed.\nAny unsaved changes will be lost.\n"
      "Please confirm.",
      " será fechado à força.\nQualquer mudança não salva será perdida.\n"
      "Confirme, por favor." },
    { "Yes", "Sim" },
    { "No", "Não" },
    { "Restore Default Launchers", "Restaurar lançadores padrão" },
    { "Dock-app", "Dockapp" },
    { "Workspace", "Área" },
    { "Preferences", "Preferências" },
    { "Theme", "Tema" },
    { "Default Colors", "Cores padrão" },
    { "Green LCD (Classic)", "LCD verde (clássico)" },
    { "Amber", "Âmbar" },
    { "Blue", "Azul" },
    { "NeXT Grey", "Cinza NeXT" },
    { "High Contrast", "Alto contraste" },
    { "Every Half Second", "A cada meio segundo" },
    { "Every Second", "A cada segundo" },
    { "Every 2 Seconds", "A cada 2 segundos" },
    { "Every 5 Seconds", "A cada 5 segundos" },
    { "Filled", "Preenchido" },
    { "Line Only", "Só a linha" },
    { "Nothing", "Nada" },
    { "Task Manager", "Gerenciador de tarefas" },
    { "System Monitor", "Monitor do sistema" },
    { "htop in a Terminal", "htop no terminal" },
    { "Open Calendar", "Abrir calendário" },
    { "Clock Style", "Estilo do relógio" },
    { "Analog", "Analógico" },
    { "12-Hour Clock (AM/PM)", "Relógio de 12 horas (AM/PM)" },
    { "Show Seconds", "Mostrar segundos" },
    { "Blinking Colon", "Dois-pontos piscando" },
    { "Calendar Style", "Estilo do calendário" },
    { "Paper Sheet", "Folha de papel" },
    { "Digital Display", "Visor digital" },
    { "Show the Year", "Mostrar o ano" },
    { "Time Zone", "Fuso horário" },
    { "Local Time", "Hora local" },
    { "Lisbon", "Lisboa" },
    { "London", "Londres" },
    { "New York", "Nova York" },
    { "Tokyo", "Tóquio" },
    { "On Click", "Ao clicar" },
    { "GNOME Calendar", "Calendário do GNOME" },
    { "Thunderbird Calendar", "Calendário do Thunderbird" },
    { "Calendar in a Terminal", "Calendário no terminal" },
    { "Open Task Manager", "Abrir gerenciador de tarefas" },
    { "Show", "Mostrar" },
    { "Each Core", "Cada núcleo" },
    { "Total Usage", "Uso total" },
    { "Graph Style", "Estilo do gráfico" },
    { "Show Percentage", "Mostrar porcentagem" },
    { "Update", "Atualizar" },
    { "Show Usage", "Mostrar uso" },
    { "Units", "Unidades" },
    { "Percent", "Porcentagem" },
    { "Show Swap", "Mostrar swap" },
    { "Open Network Settings", "Abrir configurações de rede" },
    { "All Interfaces", "Todas as interfaces" },
    { "Show Text", "Mostrar texto" },
    { "Text", "Texto" },
    { "Graph Scale", "Escala do gráfico" },
    { "Download / Upload", "Download / upload" },
    { "Bytes per Second", "Bytes por segundo" },
    { "Bits per Second", "Bits por segundo" },
    { "Network Connections", "Conexões de rede" },
    { "Network Information", "Informações de rede" },
    { "nmtui in a Terminal", "nmtui no terminal" },
    { "Mute / Unmute", "Silenciar / reativar som" },
    { "Open Volume Control", "Abrir controle de volume" },
    { "Click Mutes", "Clique silencia" },
    { "Wheel Changes Volume", "Roda muda o volume" },
    { "Volume Step", "Passo do volume" },
    { "Maximum Volume", "Volume máximo" },
    { "Volume Control", "Controle de volume" },
    { "PulseAudio Volume Control", "Controle de volume do PulseAudio" },
    { "PulseAudio Volume Control (Qt)", "Controle de volume do PulseAudio (Qt)" },
    { "PipeWire Volume Control", "Controle de volume do PipeWire" },
    { NULL, NULL }
};

/** Locale set by @ref wlmaker_i18n_set_locale. */
static const char *_wlmaker_i18n_locale_ptr = NULL;

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
const char *wlmaker_i18n(const char *text_ptr)
{
    const char *locale_ptr = _wlmaker_i18n_locale_ptr;
    // Like gettext: LANGUAGE (a list, the first counts here), then the
    // locale categories.
    static const char *vars[] = { "LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG" };
    for (size_t i = 0; NULL == locale_ptr && i < sizeof(vars) / sizeof(vars[0]);
         ++i) {
        const char *v = getenv(vars[i]);
        if (NULL != v && '\0' != *v) locale_ptr = v;
    }
    if (NULL == locale_ptr) return text_ptr;

    // Portuguese: pt_BR, and pt (without a better match).
    if (0 != strncmp(locale_ptr, "pt", 2) ||
        (locale_ptr[2] != '\0' && locale_ptr[2] != '_' &&
         locale_ptr[2] != '.' && locale_ptr[2] != ':' &&
         locale_ptr[2] != '@')) return text_ptr;

    for (const wlmaker_i18n_entry_t *e = _wlmaker_i18n_pt_br;
         NULL != e->text_ptr; ++e) {
        if (0 == strcmp(e->text_ptr, text_ptr)) return e->translation_ptr;
    }
    return text_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_i18n_set_locale(const char *locale_ptr)
{
    _wlmaker_i18n_locale_ptr = locale_ptr;
}

/* == Unit tests =========================================================== */

static void test_translate(bs_test_t *test_ptr);

/** Test cases. */
static const bs_test_case_t _wlmaker_i18n_test_cases[] = {
    { true, "translate", test_translate },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_i18n_test_set = BS_TEST_SET(
    true, "i18n", _wlmaker_i18n_test_cases);

/* ------------------------------------------------------------------------- */
/** Translates for pt_BR, keeps English otherwise. */
void test_translate(bs_test_t *test_ptr)
{
    wlmaker_i18n_set_locale("pt_BR.UTF-8");
    BS_TEST_VERIFY_STREQ(test_ptr, "Mover para cima", wlmaker_i18n("Move Up"));
    BS_TEST_VERIFY_STREQ(test_ptr, "Restaurar lançadores padrão",
                         wlmaker_i18n("Restore Default Launchers"));
    BS_TEST_VERIFY_STREQ(test_ptr, "Unknown", wlmaker_i18n("Unknown"));
    BS_TEST_VERIFY_STREQ(test_ptr, "Preferências", wlmaker_i18n("Preferences"));
    BS_TEST_VERIFY_STREQ(test_ptr, "Mostrar segundos",
                         wlmaker_i18n("Show Seconds"));

    wlmaker_i18n_set_locale("pt");
    BS_TEST_VERIFY_STREQ(test_ptr, "Executar", wlmaker_i18n("Launch"));
    BS_TEST_VERIFY_STREQ(test_ptr, "Trazer aqui", wlmaker_i18n("Bring Here"));
    BS_TEST_VERIFY_STREQ(test_ptr, "Dock position",
                         wlmaker_i18n("Dock position"));

    wlmaker_i18n_set_locale("en_US.UTF-8");
    BS_TEST_VERIFY_STREQ(test_ptr, "Move Up", wlmaker_i18n("Move Up"));
    wlmaker_i18n_set_locale("ptx");
    BS_TEST_VERIFY_STREQ(test_ptr, "Move Up", wlmaker_i18n("Move Up"));

    // From the environment.
    wlmaker_i18n_set_locale(NULL);
    bs_test_setenv(test_ptr, "LANGUAGE", "%s", "");
    bs_test_setenv(test_ptr, "LC_ALL", "%s", "");
    bs_test_setenv(test_ptr, "LC_MESSAGES", "pt_BR.UTF-8");
    bs_test_setenv(test_ptr, "LANG", "en_US.UTF-8");
    BS_TEST_VERIFY_STREQ(test_ptr, "Terminar", wlmaker_i18n("Kill"));
    bs_test_setenv(test_ptr, "LC_ALL", "C");
    BS_TEST_VERIFY_STREQ(test_ptr, "Kill", wlmaker_i18n("Kill"));
}

/* == End of i18n.c ======================================================== */
