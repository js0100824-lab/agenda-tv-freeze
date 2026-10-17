// Agenda TV Scene Freeze - plugin para OBS Studio 32.x
// Pausa video/audio de las escenas que salen del aire y reanuda al volver.
// No cierra, recarga ni reinicia las fuentes.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/bmem.h>

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTimer>
#include <QMetaObject>

#include <set>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()

static const char *kEvFreeze = "agendatv-freeze";
static const char *kEvResume = "agendatv-resume";

struct Config {
	bool enabled = true;
	std::set<std::string> scenes;   // escenas controladas
	std::set<std::string> excluded; // fuentes excluidas
};

struct Conn {
	obs_weak_source_t *weak;
	char *name;
};

static Config g_cfg;
static std::set<std::string> g_paused; // fuentes pausadas por el plugin
static std::vector<Conn> g_conns;
static QWidget *g_panel = nullptr;
static QTreeWidget *g_tree = nullptr;
static QCheckBox *g_enable = nullptr;
static QTimer *g_timer = nullptr;
static bool g_populating = false;

static bool is_media(const char *id)
{
	return id && (!strcmp(id, "ffmpeg_source") || !strcmp(id, "vlc_source"));
}
static bool is_browser(const char *id)
{
	return id && !strcmp(id, "browser_source");
}

/* ---------- Configuración ---------- */

static void read_arr(obs_data_t *d, const char *key, std::set<std::string> &out)
{
	obs_data_array_t *a = obs_data_get_array(d, key);
	if (!a)
		return;
	for (size_t i = 0; i < obs_data_array_count(a); i++) {
		obs_data_t *o = obs_data_array_item(a, i);
		out.insert(obs_data_get_string(o, "name"));
		obs_data_release(o);
	}
	obs_data_array_release(a);
}

static void write_arr(obs_data_t *d, const char *key, const std::set<std::string> &in)
{
	obs_data_array_t *a = obs_data_array_create();
	for (const auto &n : in) {
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "name", n.c_str());
		obs_data_array_push_back(a, o);
		obs_data_release(o);
	}
	obs_data_set_array(d, key, a);
	obs_data_array_release(a);
}

static void load_config()
{
	g_cfg = Config();
	char *p = obs_module_config_path("config.json");
	if (!p)
		return;
	obs_data_t *d = obs_data_create_from_json_file(p);
	bfree(p);
	if (!d)
		return;
	obs_data_set_default_bool(d, "enabled", true);
	g_cfg.enabled = obs_data_get_bool(d, "enabled");
	read_arr(d, "scenes", g_cfg.scenes);
	read_arr(d, "excluded", g_cfg.excluded);
	obs_data_release(d);
}

static void save_config()
{
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "enabled", g_cfg.enabled);
	write_arr(d, "scenes", g_cfg.scenes);
	write_arr(d, "excluded", g_cfg.excluded);
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path("config.json");
	if (p) {
		obs_data_save_json_safe(d, p, "tmp", "bak");
		bfree(p);
	}
	obs_data_release(d);
}

/* ---------- Fuentes de una escena ---------- */

struct Collect {
	std::vector<obs_source_t *> *out;
	int depth;
};

static bool enum_cb(obs_scene_t *, obs_sceneitem_t *item, void *p)
{
	auto *c = static_cast<Collect *>(p);
	obs_source_t *s = obs_sceneitem_get_source(item);
	if (!s)
		return true;
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, enum_cb, p);
		return true;
	}
	obs_scene_t *sub = obs_scene_from_source(s);
	if (sub) {
		if (c->depth < 4) {
			c->depth++;
			obs_scene_enum_items(sub, enum_cb, p);
			c->depth--;
		}
		return true;
	}
	const char *id = obs_source_get_unversioned_id(s);
	if (is_media(id) || is_browser(id)) {
		obs_source_t *ref = obs_source_get_ref(s);
		if (ref)
			c->out->push_back(ref);
	}
	return true;
}

// Devuelve referencias; quien llama debe liberarlas con release_all().
static std::vector<obs_source_t *> children_of(obs_source_t *scene)
{
	std::vector<obs_source_t *> out;
	obs_scene_t *sc = obs_scene_from_source(scene);
	if (sc) {
		Collect c{&out, 0};
		obs_scene_enum_items(sc, enum_cb, &c);
	}
	return out;
}

static void release_all(std::vector<obs_source_t *> &v)
{
	for (auto *s : v)
		obs_source_release(s);
	v.clear();
}

/* ---------- Pausar / reanudar ---------- */

static void call_js(obs_source_t *src, const char *ev)
{
	proc_handler_t *ph = obs_source_get_proc_handler(src);
	if (!ph)
		return;
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "eventName", ev);
	calldata_set_string(&cd, "jsonString", "{}");
	proc_handler_call(ph, "javascript_event", &cd);
	calldata_free(&cd);
}

static void freeze_source(obs_source_t *c)
{
	std::string n = obs_source_get_name(c);
	if (g_cfg.excluded.count(n) || obs_source_active(c))
		return; // sigue al aire por otra escena
	const char *id = obs_source_get_unversioned_id(c);
	if (is_media(id)) {
		if (obs_source_media_get_state(c) == OBS_MEDIA_STATE_PLAYING) {
			obs_source_media_play_pause(c, true);
			g_paused.insert(n);
		}
	} else if (is_browser(id)) {
		call_js(c, kEvFreeze);
	}
}

static void resume_source(obs_source_t *c)
{
	std::string n = obs_source_get_name(c);
	if (g_cfg.excluded.count(n))
		return;
	const char *id = obs_source_get_unversioned_id(c);
	if (is_media(id)) {
		if (g_paused.erase(n))
			obs_source_media_play_pause(c, false);
	} else if (is_browser(id) && obs_source_active(c)) {
		call_js(c, kEvResume);
	}
}

static void freeze_scene(const std::string &name)
{
	if (!g_cfg.enabled || !g_cfg.scenes.count(name))
		return;
	obs_source_t *scene = obs_get_source_by_name(name.c_str());
	if (!scene)
		return;
	if (!obs_source_active(scene)) { // por si volvió al aire enseguida
		auto kids = children_of(scene);
		for (auto *c : kids)
			freeze_source(c);
		release_all(kids);
	}
	obs_source_release(scene);
}

static void resume_scene(const std::string &name)
{
	if (!g_cfg.enabled || !g_cfg.scenes.count(name))
		return;
	obs_source_t *scene = obs_get_source_by_name(name.c_str());
	if (!scene)
		return;
	if (obs_source_active(scene)) {
		auto kids = children_of(scene);
		for (auto *c : kids)
			resume_source(c);
		release_all(kids);
	}
	obs_source_release(scene);
}

static void apply_state()
{
	for (const auto &n : std::set<std::string>(g_cfg.scenes)) {
		freeze_scene(n);
		resume_scene(n);
	}
}

// Reanuda todo lo que el plugin tenía pausado (al desactivar o cambiar la lista)
static void resume_all()
{
	for (const auto &n : std::set<std::string>(g_paused)) {
		obs_source_t *s = obs_get_source_by_name(n.c_str());
		if (s) {
			obs_source_media_play_pause(s, false);
			obs_source_release(s);
		}
	}
	g_paused.clear();
	for (const auto &sn : g_cfg.scenes) {
		obs_source_t *scene = obs_get_source_by_name(sn.c_str());
		if (!scene)
			continue;
		auto kids = children_of(scene);
		for (auto *c : kids)
			if (is_browser(obs_source_get_unversioned_id(c)))
				call_js(c, kEvResume);
		release_all(kids);
		obs_source_release(scene);
	}
}

// Sincroniza las páginas de navegador cada pocos segundos (cubre cargas tardías)
static void sync_browsers()
{
	if (!g_cfg.enabled)
		return;
	for (const auto &sn : g_cfg.scenes) {
		obs_source_t *scene = obs_get_source_by_name(sn.c_str());
		if (!scene)
			continue;
		auto kids = children_of(scene);
		for (auto *c : kids) {
			if (!is_browser(obs_source_get_unversioned_id(c)))
				continue;
			if (g_cfg.excluded.count(obs_source_get_name(c)))
				continue;
			call_js(c, obs_source_active(c) ? kEvResume : kEvFreeze);
		}
		release_all(kids);
		obs_source_release(scene);
	}
}

/* ---------- Señales de las escenas ---------- */

static void post_scene_event(const char *name, bool active)
{
	if (!g_panel)
		return;
	std::string n = name;
	QMetaObject::invokeMethod(
		g_panel,
		[n, active]() {
			if (active)
				resume_scene(n);
			else
				freeze_scene(n);
		},
		Qt::QueuedConnection);
}

static void sig_activate(void *data, calldata_t *) { post_scene_event((const char *)data, true); }
static void sig_deactivate(void *data, calldata_t *) { post_scene_event((const char *)data, false); }

static void disconnect_all()
{
	for (auto &c : g_conns) {
		obs_source_t *s = obs_weak_source_get_source(c.weak);
		if (s) {
			signal_handler_t *sh = obs_source_get_signal_handler(s);
			signal_handler_disconnect(sh, "activate", sig_activate, c.name);
			signal_handler_disconnect(sh, "deactivate", sig_deactivate, c.name);
			obs_source_release(s);
		}
		obs_weak_source_release(c.weak);
		bfree(c.name);
	}
	g_conns.clear();
}

static void reconnect()
{
	disconnect_all();
	for (const auto &n : g_cfg.scenes) {
		obs_source_t *s = obs_get_source_by_name(n.c_str());
		if (!s)
			continue;
		if (obs_scene_from_source(s)) {
			signal_handler_t *sh = obs_source_get_signal_handler(s);
			char *d = bstrdup(n.c_str());
			signal_handler_connect(sh, "activate", sig_activate, d);
			signal_handler_connect(sh, "deactivate", sig_deactivate, d);
			g_conns.push_back({obs_source_get_weak_source(s), d});
		}
		obs_source_release(s);
	}
}

/* ---------- Interfaz ---------- */

static QString status_for(obs_source_t *c)
{
	const char *id = obs_source_get_unversioned_id(c);
	obs_data_t *st = obs_source_get_settings(c);
	QString s, w;
	if (is_media(id)) {
		switch (obs_source_media_get_state(c)) {
		case OBS_MEDIA_STATE_PLAYING: s = "Reproduciendo"; break;
		case OBS_MEDIA_STATE_PAUSED:
			s = g_paused.count(obs_source_get_name(c)) ? "Pausada por Agenda TV" : "Pausada";
			break;
		case OBS_MEDIA_STATE_ENDED: s = "Terminada"; break;
		case OBS_MEDIA_STATE_STOPPED: s = "Detenida"; break;
		default: s = "-"; break;
		}
		if (obs_data_get_bool(st, "close_when_inactive"))
			w += "  [!] se cierra al salir del aire";
		if (obs_data_get_bool(st, "restart_on_activate"))
			w += "  [!] se reinicia al volver";
	} else {
		s = obs_source_active(c) ? "Navegador: al aire" : "Navegador: en pausa (página compatible)";
		if (obs_data_get_bool(st, "shutdown"))
			w += "  [!] se apaga al ocultarse";
		if (obs_data_get_bool(st, "restart_when_active"))
			w += "  [!] se recarga al volver";
	}
	obs_data_release(st);
	return s + w;
}

static void refresh_status()
{
	if (!g_tree)
		return;
	for (int i = 0; i < g_tree->topLevelItemCount(); i++) {
		auto *top = g_tree->topLevelItem(i);
		for (int j = 0; j < top->childCount(); j++) {
			auto *it = top->child(j);
			obs_source_t *s = obs_get_source_by_name(it->text(0).toUtf8().constData());
			if (s) {
				it->setText(1, status_for(s));
				obs_source_release(s);
			}
		}
	}
}

static void populate()
{
	if (!g_tree)
		return;
	g_populating = true;
	g_tree->clear();
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *sc = scenes.sources.array[i];
		std::string sn = obs_source_get_name(sc);
		auto *top = new QTreeWidgetItem(g_tree);
		top->setText(0, QString::fromStdString(sn));
		top->setFlags(top->flags() | Qt::ItemIsUserCheckable);
		top->setCheckState(0, g_cfg.scenes.count(sn) ? Qt::Checked : Qt::Unchecked);
		top->setText(1, obs_source_active(sc) ? "AL AIRE" : "fuera del aire");
		auto kids = children_of(sc);
		for (auto *c : kids) {
			std::string cn = obs_source_get_name(c);
			auto *it = new QTreeWidgetItem(top);
			it->setText(0, QString::fromStdString(cn));
			it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
			it->setCheckState(0, g_cfg.excluded.count(cn) ? Qt::Unchecked : Qt::Checked);
			it->setText(1, status_for(c));
		}
		release_all(kids);
	}
	obs_frontend_source_list_free(&scenes);
	g_tree->expandAll();
	g_populating = false;
}

static void fix_options()
{
	for (const auto &sn : g_cfg.scenes) {
		obs_source_t *scene = obs_get_source_by_name(sn.c_str());
		if (!scene)
			continue;
		auto kids = children_of(scene);
		for (auto *c : kids) {
			if (g_cfg.excluded.count(obs_source_get_name(c)))
				continue;
			const char *id = obs_source_get_unversioned_id(c);
			obs_data_t *st = obs_source_get_settings(c);
			bool ch = false;
			const char *keys_media[] = {"close_when_inactive", "restart_on_activate"};
			const char *keys_web[] = {"shutdown", "restart_when_active"};
			for (const char *k : is_media(id) ? keys_media : keys_web) {
				if (obs_data_get_bool(st, k)) {
					obs_data_set_bool(st, k, false);
					ch = true;
				}
			}
			if (ch)
				obs_source_update(c, st); // (una fuente de navegador puede recargarse una vez)
			obs_data_release(st);
		}
		release_all(kids);
		obs_source_release(scene);
	}
	populate();
}

static QWidget *create_panel()
{
	auto *w = new QWidget();
	auto *lay = new QVBoxLayout(w);

	g_enable = new QCheckBox("Activar Agenda TV Freeze");
	g_enable->setChecked(g_cfg.enabled);
	lay->addWidget(g_enable);

	auto *hint = new QLabel("Marca las escenas a controlar. Desmarca una fuente para excluirla.");
	hint->setWordWrap(true);
	lay->addWidget(hint);

	g_tree = new QTreeWidget();
	g_tree->setColumnCount(2);
	g_tree->setHeaderLabels({"Escena / Fuente", "Estado"});
	g_tree->setColumnWidth(0, 170);
	lay->addWidget(g_tree, 1);

	auto *row = new QHBoxLayout();
	auto *btnRefresh = new QPushButton("Actualizar");
	auto *btnFix = new QPushButton("Ajustar opciones [!]");
	btnFix->setToolTip("Desactiva las opciones que cierran, reinician o recargan las fuentes controladas");
	row->addWidget(btnRefresh);
	row->addWidget(btnFix);
	lay->addLayout(row);

	QObject::connect(g_enable, &QCheckBox::toggled, w, [](bool on) {
		g_cfg.enabled = on;
		save_config();
		if (on)
			apply_state();
		else
			resume_all();
	});
	QObject::connect(g_tree, &QTreeWidget::itemChanged, w, [](QTreeWidgetItem *it, int col) {
		if (g_populating || col != 0)
			return;
		std::string name = it->text(0).toUtf8().constData();
		bool on = it->checkState(0) == Qt::Checked;
		if (!it->parent()) {
			if (on)
				g_cfg.scenes.insert(name);
			else
				g_cfg.scenes.erase(name);
		} else {
			if (on)
				g_cfg.excluded.erase(name);
			else
				g_cfg.excluded.insert(name);
		}
		save_config();
		reconnect();
		resume_all();
		apply_state();
	});
	QObject::connect(btnRefresh, &QPushButton::clicked, w, []() { populate(); });
	QObject::connect(btnFix, &QPushButton::clicked, w, []() { fix_options(); });

	g_timer = new QTimer(w);
	QObject::connect(g_timer, &QTimer::timeout, w, []() {
		sync_browsers();
		refresh_status();
	});
	g_timer->start(2000);
	return w;
}

/* ---------- Eventos de OBS y ciclo de vida ---------- */

static void on_frontend_event(enum obs_frontend_event e, void *)
{
	switch (e) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_EXIT:
		if (g_timer)
			g_timer->stop();
		disconnect_all();
		g_paused.clear();
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		if (g_timer && !g_timer->isActive())
			g_timer->start(2000);
		reconnect();
		populate();
		apply_state();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	load_config();
	g_panel = create_panel();
	obs_frontend_add_dock_by_id("agendatv-scene-freeze", "Agenda TV Freeze", g_panel);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	blog(LOG_INFO, "[Agenda TV Freeze] cargado");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	disconnect_all();
	g_paused.clear();
	g_panel = nullptr;
	g_tree = nullptr;
	g_timer = nullptr;
}
