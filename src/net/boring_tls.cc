/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2015 Cloudius Systems
 */

#ifdef TP_BUILD

#include <openssl/x509.h>
#include <system_error>

#include <seastar/core/loop.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/file.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/print.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/net/tls.hh>
#include <seastar/net/stack.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/variant_utils.hh>

#include <boost/range/iterator_range.hpp>
#include <boost/range/adaptor/map.hpp>

#include "../core/fsnotify.hh"

namespace seastar {

class net::get_impl {
public:
    static std::unique_ptr<connected_socket_impl> get(connected_socket s) {
        return std::move(s._csi);
    }
};

class blob_wrapper {
public:
    blob_wrapper(const tls::blob& in) {}
};

class gnutlsinit {
public:
    gnutlsinit() {
        /*gnutls_global_init();*/
    }
    ~gnutlsinit() {
        /*gnutls_global_deinit();*/
    }
};

// Helper to ensure gnutls legacy init
// is handled properly with regards to
// object life spans. Could be better,
// this version will not destroy the
// gnutls stack until process exit.
class gnutlsobj {
public:
    gnutlsobj() {
        static gnutlsinit init;
    }
};

// Helper
struct file_info {
    sstring filename;
    std::chrono::system_clock::time_point modified;
};

struct file_result {
    temporary_buffer<char> buf;
    file_info file;
    operator temporary_buffer<char>&&() && {
        return std::move(buf);
    }
};

static future<file_result> read_fully(const sstring& name, const sstring& what) {
    return open_file_dma(name, open_flags::ro).then([name = name](file f) mutable {
        return do_with(std::move(f), [name = std::move(name)](file& f) mutable {
            return f.stat().then([&f, name = std::move(name)](struct stat s) mutable {
                return f.dma_read_bulk<char>(0, s.st_size).then([s, name = std::move(name)](temporary_buffer<char> buf) mutable {
                    return file_result{ std::move(buf), file_info{ 
                        std::move(name), std::chrono::system_clock::from_time_t(s.st_mtim.tv_sec) +
                            std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(s.st_mtim.tv_nsec))
                    } };
                });
            }).finally([&f]() {
                return f.close();
            });
        });
    }).handle_exception([name = name, what = what](std::exception_ptr ep) -> future<file_result> {
       try {
           std::rethrow_exception(std::move(ep));
       } catch (...) {
           std::throw_with_nested(std::runtime_error(sstring("Could not read ") + what + " " + name));
       }
    });
}

// Note: we are not using gnutls++ interfaces, mainly because we
// want to keep _our_ interface reasonably non-gnutls (well...)
// and once we get to this level, their abstractions don't help
// that much anyway. And they are sooo c++98...
class gnutls_error_category : public std::error_category {
public:
    constexpr gnutls_error_category() noexcept : std::error_category{} {}
    const char * name() const noexcept {
        return "GnuTLS";
    }
    std::string message(int error) const {
        return "xxx";
    }
};

static const gnutls_error_category glts_errorc;

// Checks a gnutls return value.
// < 0 -> error.
static void gtls_chk(int res) {
    if (res < 0) {
        throw std::system_error(res, glts_errorc);
    }
}

namespace {

// helper for gnutls-functions for receiving a string
// arguments
//  func - the gnutls function that is returning a string (e.g. gnutls_x509_crt_get_issuer_dn)
//  args - the arguments to func that come before the char array's ptr and size args
// returns
//  pair<int, string> - [gnutls error code, extracted string],
//                      in case of no errors, the error code is zero
static auto get_gtls_string = [](auto func, auto... args) noexcept {
    size_t size = 0;
    int ret = func(args..., nullptr, &size);

    sstring res(sstring::initialized_later{}, size - 1);
    return std::make_pair(ret, res);
};

}

class tls::dh_params::impl : gnutlsobj {
public:
    impl() {}
    impl(level lvl) {}
    impl(const blob& pkcs3, x509_crt_format fmt) {}
    impl(const impl& v) {}
    ~impl() = default;
};

tls::dh_params::dh_params(level lvl) : _impl(std::make_unique<impl>(lvl))
{}

tls::dh_params::dh_params(const blob& b, x509_crt_format fmt)
        : _impl(std::make_unique<impl>(b, fmt)) {
}

tls::dh_params::~dh_params() {
}

tls::dh_params::dh_params(dh_params&&) noexcept = default;
tls::dh_params& tls::dh_params::operator=(dh_params&&) noexcept = default;

future<tls::dh_params> tls::dh_params::from_file(
        const sstring& filename, x509_crt_format fmt) {
    return read_fully(filename, "dh parameters").then([fmt](temporary_buffer<char> buf) {
        return make_ready_future<dh_params>(dh_params(blob(buf.get()), fmt));
    });
}

class tls::x509_cert::impl : gnutlsobj {
public:
    impl() {}
    impl(const blob& b, x509_crt_format fmt) : impl() {}
    ~impl() {}

private:
};

tls::x509_cert::x509_cert(shared_ptr<impl> impl)
        : _impl(std::move(impl)) {
}

tls::x509_cert::x509_cert(const blob& b, x509_crt_format fmt)
        : x509_cert(::seastar::make_shared<impl>(b, fmt)) {
}

future<tls::x509_cert> tls::x509_cert::from_file(
        const sstring& filename, x509_crt_format fmt) {
    return read_fully(filename, "x509 certificate").then([fmt](temporary_buffer<char> buf) {
        return make_ready_future<x509_cert>(x509_cert(blob(buf.get()), fmt));
    });
}

class tls::certificate_credentials::impl: public gnutlsobj {
public:
    impl() {}
    ~impl() {}

    void set_x509_trust(const blob& b, x509_crt_format fmt) {
        blob_wrapper w(b);
    }
    void set_x509_crl(const blob& b, x509_crt_format fmt) {
        blob_wrapper w(b);
    }
    void set_x509_key(const blob& cert, const blob& key, x509_crt_format fmt) {
        blob_wrapper w1(cert);
        blob_wrapper w2(key);
    }
    void set_simple_pkcs12(const blob& b, x509_crt_format fmt,
            const sstring& password) {
        blob_wrapper w(b);
    }
    void dh_params(const tls::dh_params& dh) {
    }

    future<> set_system_trust() {
        return async([this] {
            _load_system_trust = false; // should only do once, for whatever reason
        });
    }
    void set_client_auth(client_auth ca) {
        _client_auth = ca;
    }
    client_auth get_client_auth() const {
        return _client_auth;
    }
    void set_priority_string(const sstring& prio) {
        const char * err = prio.c_str();
        try {
        } catch (...) {
            std::throw_with_nested(std::invalid_argument(std::string("Could not set priority: ") + err));
        }
    }
    void set_dn_verification_callback(dn_callback cb) {
        _dn_callback = std::move(cb);
    }
private:
    friend class credentials_builder;
    friend class session;

    bool need_load_system_trust() const {
        return _load_system_trust;
    }
    future<> maybe_load_system_trust() {
        return with_semaphore(_system_trust_sem, 1, [this] {
            if (!_load_system_trust) {
                return make_ready_future();
            }
            return set_system_trust();
        });
    }

    std::unique_ptr<tls::dh_params::impl> _dh_params;
    client_auth _client_auth = client_auth::NONE;
    bool _load_system_trust = false;
    semaphore _system_trust_sem {1};
    dn_callback _dn_callback;
};

tls::certificate_credentials::certificate_credentials()
        : _impl(make_shared<impl>()) {
}

tls::certificate_credentials::~certificate_credentials() {
}

tls::certificate_credentials::certificate_credentials(
        certificate_credentials&&) noexcept = default;
tls::certificate_credentials& tls::certificate_credentials::operator=(
        certificate_credentials&&) noexcept = default;

void tls::certificate_credentials::set_x509_trust(const blob& b,
        x509_crt_format fmt) {
    _impl->set_x509_trust(b, fmt);
}

void tls::certificate_credentials::set_x509_crl(const blob& b,
        x509_crt_format fmt) {
    _impl->set_x509_crl(b, fmt);

}
void tls::certificate_credentials::set_x509_key(const blob& cert,
        const blob& key, x509_crt_format fmt) {
    _impl->set_x509_key(cert, key, fmt);
}

void tls::certificate_credentials::set_simple_pkcs12(const blob& b,
        x509_crt_format fmt, const sstring& password) {
    _impl->set_simple_pkcs12(b, fmt, password);
}

future<> tls::abstract_credentials::set_x509_trust_file(
        const sstring& cafile, x509_crt_format fmt) {
    return read_fully(cafile, "trust file").then([this, fmt](temporary_buffer<char> buf) {
        set_x509_trust(blob(buf.get(), buf.size()), fmt);
    });
}

future<> tls::abstract_credentials::set_x509_crl_file(
        const sstring& crlfile, x509_crt_format fmt) {
    return read_fully(crlfile, "crl file").then([this, fmt](temporary_buffer<char> buf) {
        set_x509_crl(blob(buf.get(), buf.size()), fmt);
    });
}

future<> tls::abstract_credentials::set_x509_key_file(
        const sstring& cf, const sstring& kf, x509_crt_format fmt) {
    return read_fully(cf, "certificate file").then([this, fmt, kf = kf](temporary_buffer<char> buf) {
        return read_fully(kf, "key file").then([this, fmt, buf = std::move(buf)](temporary_buffer<char> buf2) {
                    set_x509_key(blob(buf.get(), buf.size()), blob(buf2.get(), buf2.size()), fmt);
                });
    });
}

future<> tls::abstract_credentials::set_simple_pkcs12_file(
        const sstring& pkcs12file, x509_crt_format fmt,
        const sstring& password) {
    return read_fully(pkcs12file, "pkcs12 file").then([this, fmt, password = password](temporary_buffer<char> buf) {
        set_simple_pkcs12(blob(buf.get(), buf.size()), fmt, password);
    });
}

future<> tls::certificate_credentials::set_system_trust() {
    return _impl->set_system_trust();
}

void tls::certificate_credentials::set_priority_string(const sstring& prio) {
    _impl->set_priority_string(prio);
}

void tls::certificate_credentials::set_dn_verification_callback(dn_callback cb) {
    _impl->set_dn_verification_callback(std::move(cb));
}

tls::server_credentials::server_credentials()
{}

tls::server_credentials::server_credentials(shared_ptr<dh_params> dh)
    : server_credentials(*dh)
{}

tls::server_credentials::server_credentials(const dh_params& dh) {
    _impl->dh_params(dh);
}

tls::server_credentials::server_credentials(server_credentials&&) noexcept = default;
tls::server_credentials& tls::server_credentials::operator=(
        server_credentials&&) noexcept = default;

void tls::server_credentials::set_client_auth(client_auth ca) {
    _impl->set_client_auth(ca);
}

static const sstring dh_level_key = "dh_level";
static const sstring x509_trust_key = "x509_trust";
static const sstring x509_crl_key = "x509_crl";
static const sstring x509_key_key = "x509_key";
static const sstring pkcs12_key = "pkcs12";
static const sstring system_trust = "system_trust";

using buffer_type = std::basic_string<tls::blob::value_type, tls::blob::traits_type, std::allocator<tls::blob::value_type>>;

struct x509_simple {
    buffer_type data;
    tls::x509_crt_format format;
    file_info file;
};

struct x509_key {
    buffer_type cert;
    buffer_type key;
    tls::x509_crt_format format;
    file_info cert_file;
    file_info key_file;
};

struct pkcs12_simple {
    buffer_type data;
    tls::x509_crt_format format;
    sstring password;
    file_info file;
};

void tls::credentials_builder::set_dh_level(dh_params::level level) {
    _blobs.emplace(dh_level_key, level);
}

void tls::credentials_builder::set_x509_trust(const blob& b, x509_crt_format fmt) {
    _blobs.emplace(x509_trust_key, x509_simple{ std::string(b), fmt });
}

void tls::credentials_builder::set_x509_crl(const blob& b, x509_crt_format fmt) {
    _blobs.emplace(x509_crl_key, x509_simple{ std::string(b), fmt });
}

void tls::credentials_builder::set_x509_key(const blob& cert, const blob& key, x509_crt_format fmt) {
    _blobs.emplace(x509_key_key, x509_key { std::string(cert), std::string(key), fmt });
}

void tls::credentials_builder::set_simple_pkcs12(const blob& b, x509_crt_format fmt, const sstring& password) {
    _blobs.emplace(pkcs12_key, pkcs12_simple{std::string(b), fmt, password });
}

static buffer_type to_buffer(const temporary_buffer<char>& buf) {
    return buffer_type(buf.get(), buf.get() + buf.size());
}

future<> tls::credentials_builder::set_x509_trust_file(const sstring& cafile, x509_crt_format fmt) {
    return read_fully(cafile, "trust file").then([this, fmt](file_result f) {
        _blobs.emplace(x509_trust_key, x509_simple{ to_buffer(f.buf), fmt, std::move(f.file) });
    });
}

future<> tls::credentials_builder::set_x509_crl_file(const sstring& crlfile, x509_crt_format fmt) {
    return read_fully(crlfile, "crl file").then([this, fmt](file_result f) {
        _blobs.emplace(x509_crl_key, x509_simple{ to_buffer(f.buf), fmt, std::move(f.file) });
    });
}

future<> tls::credentials_builder::set_x509_key_file(const sstring& cf, const sstring& kf, x509_crt_format fmt) {
    return read_fully(cf, "certificate file").then([this, fmt, kf = kf](file_result cf) {
        return read_fully(kf, "key file").then([this, fmt, cf = std::move(cf)](file_result kf) {
            _blobs.emplace(x509_key_key, x509_key{ to_buffer(cf.buf), to_buffer(kf.buf), fmt, std::move(cf.file), std::move(kf.file) });
        });
    });
}

future<> tls::credentials_builder::set_simple_pkcs12_file(const sstring& pkcs12file, x509_crt_format fmt, const sstring& password) {
    return read_fully(pkcs12file, "pkcs12 file").then([this, fmt, password = password](file_result f) {
        _blobs.emplace(pkcs12_key, pkcs12_simple{ to_buffer(f.buf), fmt, password, std::move(f.file) });
    });
}

future<> tls::credentials_builder::set_system_trust() {
    // TODO / Caveat:
    // We cannot actually issue a loading of system trust here,
    // because we have no actual tls context.
    // And we probably _don't want to get into the guessing game
    // of where the system trust cert chains are, since this is
    // super distro dependent, and usually compiled into the library.
    // Pretent it is raining, and just set a flag.
    // Leave the function returning future, so if we change our
    // minds and want to do explicit loading, we can...
    _blobs.emplace(system_trust, true);
    return make_ready_future();
}

void tls::credentials_builder::set_client_auth(client_auth auth) {
    _client_auth = auth;
}

void tls::credentials_builder::set_priority_string(const sstring& prio) {
    _priority = prio;
}

template<typename Blobs, typename Visitor>
static void visit_blobs(Blobs& blobs, Visitor&& visitor) {
    auto visit = [&](const sstring& key, auto* vt) {
        auto tr = blobs.equal_range(key);
        for (auto& p : boost::make_iterator_range(tr.first, tr.second)) {
            auto* v = boost::any_cast<std::decay_t<decltype(*vt)>>(&p.second);
            visitor(key, *v);
        }
    };
    visit(x509_trust_key, static_cast<x509_simple*>(nullptr));
    visit(x509_crl_key, static_cast<x509_simple*>(nullptr));
    visit(x509_key_key, static_cast<x509_key*>(nullptr));
    visit(pkcs12_key, static_cast<pkcs12_simple*>(nullptr));
}

void tls::credentials_builder::apply_to(certificate_credentials& creds) const {
    // Could potentially be templated down, but why bother...
    visit_blobs(_blobs, make_visitor(
        [&](const sstring& key, const x509_simple& info) {
            if (key == x509_trust_key) {
                creds.set_x509_trust(info.data, info.format);
            } else if (key == x509_crl_key) {
                creds.set_x509_crl(info.data, info.format);
            }
        },
        [&](const sstring&, const x509_key& info) {
            creds.set_x509_key(info.cert, info.key, info.format);
        },
        [&](const sstring&, const pkcs12_simple& info) {
            creds.set_simple_pkcs12(info.data, info.format, info.password);
        }
    ));

    // TODO / Caveat:
    // We cannot do this immediately, because we are not a continuation, and
    // potentially blocking calls are a no-no.
    // Doing this detached would be indeterministic, so set a flag in
    // credentials, and do actual loading in first handshake (see session)
    if (_blobs.count(system_trust)) {
        creds._impl->_load_system_trust = true;
    }

    if (!_priority.empty()) {
        creds.set_priority_string(_priority);
    }

    creds._impl->set_client_auth(_client_auth);
}

shared_ptr<tls::certificate_credentials> tls::credentials_builder::build_certificate_credentials() const {
    auto creds = make_shared<certificate_credentials>();
    apply_to(*creds);
    return creds;
}

shared_ptr<tls::server_credentials> tls::credentials_builder::build_server_credentials() const {
    auto i = _blobs.find(dh_level_key);
    if (i == _blobs.end()) {
#if GNUTLS_VERSION_NUMBER < 0x030600
        throw std::invalid_argument("No DH level set");
#else
        auto creds = make_shared<server_credentials>();
        apply_to(*creds);
        return creds;
#endif
    }
    auto creds = make_shared<server_credentials>(dh_params(boost::any_cast<dh_params::level>(i->second)));
    apply_to(*creds);
    return creds;
}

using namespace std::chrono_literals;

class tls::reloadable_credentials_base {
public:
    using delay_type = std::chrono::milliseconds;
    static inline constexpr delay_type default_tolerance = 500ms;

    class reloading_builder
        : public credentials_builder
        , public enable_shared_from_this<reloading_builder>
    {
    public:
        using time_point = std::chrono::system_clock::time_point;

        reloading_builder(credentials_builder b, reload_callback cb, reloadable_credentials_base* creds, delay_type delay)
            : credentials_builder(std::move(b))
            , _cb(std::move(cb))
            , _creds(creds)
            , _delay(delay)
        {}
        future<> init() {
            std::vector<future<>> futures;
            visit_blobs(_blobs, make_visitor(
                [&](const sstring&, const x509_simple& info) {
                    _all_files.emplace(info.file.filename);
                },
                [&](const sstring&, const x509_key& info) {
                    _all_files.emplace(info.cert_file.filename);
                    _all_files.emplace(info.key_file.filename);
                },
                [&](const sstring&, const pkcs12_simple& info) {
                    _all_files.emplace(info.file.filename);
                }
            ));
            return parallel_for_each(_all_files, [this](auto& f) {
                if (!f.empty()) {
                    return add_watch(f).discard_result();
                }
                return make_ready_future<>();
            }).finally([me = shared_from_this()] {});
        }
        void start() {
            // run the loop in a thread. makes code almost readable.
            (void)async(std::bind(&reloading_builder::run, this)).finally([me = shared_from_this()] {});
        }
        void run() {
            while (_creds) {
                try {
                    auto events = _fsn.wait().get0();
                    if (events.empty() && _creds == nullptr) {
                        return;
                    }
                    rebuild(events);
                    _timer.cancel();
                } catch (...) {
                    if (!_timer.armed()) {
                        _timer.set_callback([this, ep = std::current_exception()]() mutable {
                            do_callback(std::move(ep));
                        });
                        _timer.arm(_delay);
                    }
                }
            }
        }
        void detach() {
            _creds = nullptr;
            _cb = {};
            _fsn.shutdown();
            _timer.cancel();
        }
    private:
        // called from seastar::thread
        void rebuild(const std::vector<fsnotifier::event>& events) {
            for (auto& e : events) {
                // don't use at. We could be getting two events for
                // same watch (mod + delete), but we only need to care
                // about one...
                auto i = _watches.find(e.id);
                if (i != _watches.end()) {
                    auto& filename = i->second.second;
                    // only add actual file watches to 
                    // query set. If this was a directory
                    // watch, the file should already be 
                    // in there.
                    if (_all_files.count(filename)) {
                        _files[filename] = e.mask;
                    }
                    _watches.erase(i);
                }
            }
            auto num_changed = 0;

            auto maybe_reload = [&](const sstring& filename, buffer_type& dst) {
                if (filename.empty() || !_files.count(filename)) {
                    return;
                }
                // #756
                // first, add a watch to nearest parent dir we
                // can find. If user deleted folders, we could end
                // up looking at modifications to root.
                // The idea is that should adding a watch to actual file
                // fail (deleted file/folder), we wait for changes to closest
                // parent. When this happens, we will retry all files
                // that have not been successfully replaced (and maybe more),
                // repeating the process. At some point, we hopefully
                // get new, current data.
                add_dir_watch(filename);
                // #756 add watch _first_. File could change while we are
                // reading this.
                try {
                    add_watch(filename).get();
                } catch (...) {
                    // let's just assume if this happens, it's because the file or folder was deleted.
                    // just ignore for now, and hope the dir watch will tell us when it is back...
                    return;
                }
                temporary_buffer<char> buf = read_fully(filename, "reloading").get0();
                dst = to_buffer(buf);
                ++num_changed;
            };
            visit_blobs(_blobs, make_visitor(
                [&](const sstring&, x509_simple& info) {
                    maybe_reload(info.file.filename, info.data);
                },
                [&](const sstring&, x509_key& info) {
                    maybe_reload(info.cert_file.filename, info.cert);
                    maybe_reload(info.key_file.filename, info.key);
                },
                [&](const sstring&, pkcs12_simple& info) {
                    maybe_reload(info.file.filename, info.data);
                }
            ));
            // only try this if anything was in fact successfully loaded.
            // if files were missing, or pairs incomplete, we can just skip.
            if (num_changed == 0) {
                return;
            }
            try {
                if (_creds) {
                    _creds->rebuild(*this);
                }
            } catch (...) {
                if (std::any_of(_files.begin(), _files.end(), [](auto& p) { return p.second == fsnotifier::flags::ignored; })) {
                    // if any file in the reload set was deleted - i.e. we have not seen a "closed" yet - assume
                    // this is a spurious reload and we'd better wait for next event - hopefully a "closed" - 
                    // and try again
                    return;
                }
                throw;
            }
            // if we got here, all files loaded, all watches were created,
            // and gnutls was ok with the content. success.
            do_callback();
            on_success();
        }
        void on_success() {
            _files.clear();
            // remove all directory watches, since we've successfully 
            // reloaded -> the file watches themselves should suffice now
            auto i = _watches.begin();
            auto e = _watches.end();
            while (i != e) {
                if (!_all_files.count(i->second.second)) {
                    i = _watches.erase(i);
                    continue;
                }
                ++i;
            }
        }
        void do_callback(std::exception_ptr ep = {}) {
            if (_cb && !_files.empty()) {
                _cb(boost::copy_range<std::unordered_set<sstring>>(_files | boost::adaptors::map_keys), std::move(ep));
            }
        }
        // called from seastar::thread
        fsnotifier::watch_token add_dir_watch(const sstring& filename) {
            auto dir = std::filesystem::path(filename).parent_path();
            for (;;) {
                try {
                    return add_watch(dir.native(), fsnotifier::flags::create_child | fsnotifier::flags::move).get0();
                } catch (...) {
                    auto parent = dir.parent_path();
                    if (parent.empty() || dir == parent) {
                        throw;
                    }
                    dir = std::move(parent);
                    continue;
                }
            }
        }
        future<fsnotifier::watch_token> add_watch(const sstring& filename, fsnotifier::flags flags = fsnotifier::flags::close_write|fsnotifier::flags::delete_self) {
            return _fsn.create_watch(filename, flags).then([this, filename = filename](fsnotifier::watch w) {
                auto t = w.token();
                // we might create multiple watches for same token in case of dirs, avoid deleting previously 
                // created one
                if (_watches.count(t)) {
                    w.release();
                } else {
                    _watches.emplace(t, std::make_pair(std::move(w), filename));
                }
                return t;
            });
        }

        reload_callback _cb;
        reloadable_credentials_base* _creds;
        fsnotifier _fsn;
        std::unordered_map<fsnotifier::watch_token, std::pair<fsnotifier::watch, sstring>> _watches;
        std::unordered_map<sstring, fsnotifier::flags> _files;
        std::unordered_set<sstring> _all_files;
        timer<> _timer;
        delay_type _delay;
    };
    reloadable_credentials_base(credentials_builder builder, reload_callback cb, delay_type delay = default_tolerance)
        : _builder(seastar::make_shared<reloading_builder>(std::move(builder), std::move(cb), this, delay))
    {
        _builder->start();
    }
    future<> init() {
        return _builder->init();
    }
    virtual ~reloadable_credentials_base() {
        _builder->detach();
    }
    virtual void rebuild(const credentials_builder&) = 0;
private:
    shared_ptr<reloading_builder> _builder;
};

template<typename Base>
class tls::reloadable_credentials : public Base, public tls::reloadable_credentials_base {
public:
    reloadable_credentials(credentials_builder builder, reload_callback cb, Base b, delay_type delay = default_tolerance)
        : Base(std::move(b))
        , tls::reloadable_credentials_base(std::move(builder), std::move(cb), delay)
    {}
    void rebuild(const credentials_builder&) override;
};

template<>
void tls::reloadable_credentials<tls::certificate_credentials>::rebuild(const credentials_builder& builder) {
    auto tmp = builder.build_certificate_credentials();
    this->_impl = std::move(tmp->_impl);
}

template<>
void tls::reloadable_credentials<tls::server_credentials>::rebuild(const credentials_builder& builder) {
    auto tmp = builder.build_server_credentials();
    this->_impl = std::move(tmp->_impl);
}

future<shared_ptr<tls::certificate_credentials>> tls::credentials_builder::build_reloadable_certificate_credentials(reload_callback cb, std::optional<std::chrono::milliseconds> tolerance) const {
    auto creds = seastar::make_shared<reloadable_credentials<tls::certificate_credentials>>(*this, std::move(cb), std::move(*build_certificate_credentials()), tolerance.value_or(reloadable_credentials_base::default_tolerance));
    return creds->init().then([creds] {
        return make_ready_future<shared_ptr<tls::certificate_credentials>>(creds);
    });
}

future<shared_ptr<tls::server_credentials>> tls::credentials_builder::build_reloadable_server_credentials(reload_callback cb, std::optional<std::chrono::milliseconds> tolerance) const {
    auto creds = seastar::make_shared<reloadable_credentials<tls::server_credentials>>(*this, std::move(cb), std::move(*build_server_credentials()), tolerance.value_or(reloadable_credentials_base::default_tolerance));
    return creds->init().then([creds] {
        return make_ready_future<shared_ptr<tls::server_credentials>>(creds);
    });
}

namespace tls {

/**
 * Session wraps gnutls session, and is the
 * actual conduit for an TLS/SSL data flow.
 *
 * We use a connected_socket and its sink/source
 * for IO. Note that we need to keep ownership
 * of these, since we handle handshake etc.
 *
 */
class session : public enable_lw_shared_from_this<session> {
public:
    enum class type
        : uint32_t {
            CLIENT = 0, SERVER = 1,
    };

    session(type t, shared_ptr<tls::certificate_credentials> creds,
            std::unique_ptr<net::connected_socket_impl> sock, sstring name = { })
            : _type(t), _sock(std::move(sock)), _creds(creds->_impl), _hostname(
                    std::move(name)), _in(_sock->source()), _out(_sock->sink()),
                    _in_sem(1), _out_sem(1), _output_pending(
                    make_ready_future<>()) { }
    session(type t, shared_ptr<certificate_credentials> creds,
            connected_socket sock, sstring name = { })
            : session(t, std::move(creds), net::get_impl::get(std::move(sock)),
                    std::move(name)) {
    }

    ~session() {
        assert(_output_pending.available());
    }

    typedef temporary_buffer<char> buf_type;

    future<> do_handshake() {
        if (_connected) {
            return make_ready_future<>();
        }
        return wait_for_output();
    }
    future<> handshake() {
        // maybe load system certificates before handshake, in case we
        // have not done so yet...
        if (_creds->need_load_system_trust()) {
            return _creds->maybe_load_system_trust().then([this] {
               return handshake();
            });
        }
        // acquire both semaphores to sync both read & write
        return with_semaphore(_in_sem, 1, [this] {
            return with_semaphore(_out_sem, 1, [this] {
                return do_handshake();
            });
        });
    }

    size_t in_avail() const {
        return _input.size();
    }
    bool eof() const {
        return _eof;
    }
    future<> wait_for_input() {
        if (!_input.empty()) {
            return make_ready_future<>();
        }
        return _in.get().then([this](buf_type buf) {
            _eof |= buf.empty();
           _input = std::move(buf);
        }).handle_exception([this](auto ep) {
           _error = ep;
           return make_exception_future(ep);
        });
    }
    future<> wait_for_output() {
        return std::exchange(_output_pending, make_ready_future()).handle_exception([this](auto ep) {
           _error = ep;
           return make_exception_future(ep);
        });
    }

    void verify() {
    }

    future<temporary_buffer<char>> get() {
        if (_error) {
            return make_exception_future<temporary_buffer<char>>(_error);
        }
        if (_shutdown || eof()) {
            return make_ready_future<temporary_buffer<char>>();
        }
        if (!_connected) {
            return handshake().then(std::bind(&session::get, this));
        }
        return with_semaphore(_in_sem, 1, std::bind(&session::do_get, this)).then([this](temporary_buffer<char> buf) {
            if (buf.empty() && !eof()) {
                // this must mean we got a re-handshake request.
                // see do_get.
                // We there clear connected flag and return empty in case
                // other side requests re-handshake. Now, someone else could have already dealt with it by the
                // time we are here (continuation reordering). In fact, someone could have dealt with
                // it and set the eof flag also, but in that case we're still eof...
                return handshake().then(std::bind(&session::get, this));
            }
            return make_ready_future<temporary_buffer<char>>(std::move(buf));
        });
    }

    future<temporary_buffer<char>> do_get() {
        return wait_for_input().then([this] {
            return do_get();
        });
    }

    typedef net::fragment* frag_iter;

    future<> do_put(frag_iter i, frag_iter e) {
        assert(_output_pending.available());
        return do_for_each(i, e, [this](net::fragment& f) {});
    }
    future<> put(net::packet p) {
        if (_error) {
            return make_exception_future<>(_error);
        }
        if (_shutdown) {
            return make_exception_future<>(std::system_error(ENOTCONN, std::system_category()));
        }
        if (!_connected) {
            return handshake().then([this, p = std::move(p)]() mutable {
               return put(std::move(p));
            });
        }
        auto i = p.fragments().begin();
        auto e = p.fragments().end();
        return with_semaphore(_out_sem, 1, std::bind(&session::do_put, this, i, e)).finally([p = std::move(p)] {});
    }

    ssize_t pull(void* dst, size_t len) {
        return 0;
    }

    future<>
    handle_error(int res) {
        _error = std::make_exception_ptr(std::system_error(res, glts_errorc));
        return make_exception_future(_error);
    }
    future<>
    handle_output_error(int res) {
        _error = std::make_exception_ptr(std::system_error(res, glts_errorc));
        // #453
        // defensively wait for output before generating the error.
        // if we have both error code and an exception in output
        // future, throw both.
        return wait_for_output().then_wrapped([this, res](auto f) {
            try {
                f.get();
                // output was ok/done, just generate error code exception
                return make_exception_future(_error);
            } catch (...) {
                std::throw_with_nested(std::system_error(res, glts_errorc));
            }
        });
    }
    future<> do_shutdown() {
        if (_error || !_connected) {
            return make_ready_future();
        }
        return wait_for_output();
    }
    future<> wait_for_eof() {
        // read records until we get an eof alert
        // since this call could time out, we must not ac
        return with_semaphore(_in_sem, 1, [this] {
            if (_error || !_connected) {
                return make_ready_future();
            }
            return repeat([this] {
                if (eof()) {
                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                }
                return do_get().then([](auto buf) {
                   return make_ready_future<stop_iteration>(stop_iteration::no);
                });
            });
        });
    }
    future<> shutdown() {
        // first, make sure any pending write is done.
        // bye handshake is a flush operation, but this
        // allows us to not pay extra attention to output state
        //
        // we only send a simple "bye" alert packet. Then we
        // read from input until we see EOF. Any other reader
        // before us will get it instead of us, and mark _eof = true
        // in which case we will be no-op.
        return with_semaphore(_out_sem, 1,
                        std::bind(&session::do_shutdown, this)).then(
                        std::bind(&session::wait_for_eof, this)).finally([me = shared_from_this()] {});
        // note moved finally clause above. It is theorethically possible
        // that we could complete do_shutdown just before the close calls 
        // below, get pre-empted, have "close()" finish, get freed, and 
        // then call wait_for_eof on stale pointer.
    }
    void close() {
        // only do once.
        if (!std::exchange(_shutdown, true)) {
            auto me = shared_from_this();
            // running in background. try to bye-handshake us nicely, but after 10s we forcefully close.
            (void)with_timeout(timer<>::clock::now() + std::chrono::seconds(10), shutdown()).finally([this] {
                _eof = true;
                try {
                    (void)_in.close().handle_exception([](std::exception_ptr) {}); // should wake any waiters
                } catch (...) {
                }
                try {
                    (void)_out.close().handle_exception([](std::exception_ptr) {});
                } catch (...) {
                }
                // make sure to wait for handshake attempt to leave semaphores. Must be in same order as
                // handshake aqcuire, because in worst case, we get here while a reader is attempting
                // re-handshake.
                return with_semaphore(_in_sem, 1, [this] {
                    return with_semaphore(_out_sem, 1, [] {});
                });
            }).then_wrapped([me = std::move(me)](future<> f) { // must keep object alive until here.
                f.ignore_ready_future();
            });
        }
    }
    // helper for sink
    future<> flush() noexcept {
        return with_semaphore(_out_sem, 1, [this] {
            return _out.flush();
        });
    }

    seastar::net::connected_socket_impl & socket() const {
        return *_sock;
    }

    struct session_ref;
private:
    type _type;

    std::unique_ptr<net::connected_socket_impl> _sock;
    shared_ptr<tls::certificate_credentials::impl> _creds;
    const sstring _hostname;
    data_source _in;
    data_sink _out;

    semaphore _in_sem, _out_sem;

    bool _eof = false;
    bool _shutdown = false;
    bool _connected = false;
    std::exception_ptr _error;

    future<> _output_pending;
    buf_type _input;
};

struct session::session_ref {
    session_ref() = default;
    session_ref(lw_shared_ptr<session> session)
                    : _session(std::move(session)) {
    }
    session_ref(session_ref&&) = default;
    session_ref(const session_ref&) = default;
    ~session_ref() {
        // This is not super pretty. But we take some care to only own sessions
        // through session_ref, and we need to initiate shutdown on "last owner",
        // since we cannot revive the session in destructor.
        if (_session && _session.use_count() == 1) {
            _session->close();
        }
    }

    session_ref& operator=(session_ref&&) = default;
    session_ref& operator=(const session_ref&) = default;

    lw_shared_ptr<session> _session;
};

class tls_connected_socket_impl : public net::connected_socket_impl, public session::session_ref {
public:
    tls_connected_socket_impl(session_ref&& sess)
        : session_ref(std::move(sess))
    {}

    class source_impl;
    class sink_impl;

    data_source source() override;
    data_sink sink() override;

    void shutdown_input() override {
        _session->close();
    }
    void shutdown_output() override {
        _session->close();
    }
    void set_nodelay(bool nodelay) override {
        _session->socket().set_nodelay(nodelay);
    }
    bool get_nodelay() const override {
        return _session->socket().get_nodelay();
    }
    void set_keepalive(bool keepalive) override {
        _session->socket().set_keepalive(keepalive);
    }
    bool get_keepalive() const override {
        return _session->socket().get_keepalive();
    }
    void set_keepalive_parameters(const net::keepalive_params& p) override {
        _session->socket().set_keepalive_parameters(p);
    }
    net::keepalive_params get_keepalive_parameters() const override {
        return _session->socket().get_keepalive_parameters();
    }
    void set_sockopt(int level, int optname, const void* data, size_t len) override {
        _session->socket().set_sockopt(level, optname, data, len);
    }
    int get_sockopt(int level, int optname, void* data, size_t len) const override {
        return _session->socket().get_sockopt(level, optname, data, len);
    }
};


class tls_connected_socket_impl::source_impl: public data_source_impl, public session::session_ref {
public:
    using session_ref::session_ref;
private:
    future<temporary_buffer<char>> get() override {
        return _session->get();
    }
    future<> close() override {
        _session->close();
        return make_ready_future<>();
    }
};

// Note: source/sink, and by extension, the in/out streams
// produced, cannot exist outside the direct life span of
// the connected_socket itself. This is consistent with
// other sockets in seastar, though I am than less fond of it...
class tls_connected_socket_impl::sink_impl: public data_sink_impl, public session::session_ref {
public:
    using session_ref::session_ref;
private:
    future<> flush() override {
        return _session->flush();
    }
    using data_sink_impl::put;
    future<> put(net::packet p) override {
        return _session->put(std::move(p));
    }
    future<> close() override {
        _session->close();
        return make_ready_future<>();
    }
};

class server_session : public net::server_socket_impl {
public:
    server_session(shared_ptr<server_credentials> creds, server_socket sock)
            : _creds(std::move(creds)), _sock(std::move(sock)) {
    }
    future<accept_result> accept() override {
        // We're not actually doing anything very SSL until we get
        // an actual connection. Then we create a "server" session
        // and wrap it up after handshaking.
        return _sock.accept().then([this](accept_result ar) {
            return wrap_server(_creds, std::move(ar.connection)).then([addr = std::move(ar.remote_address)](connected_socket s) {
                return make_ready_future<accept_result>(accept_result{std::move(s), addr});
            });
        });
    }
    void abort_accept() override  {
        _sock.abort_accept();
    }
    socket_address local_address() const override {
        return _sock.local_address();
    }
private:
    shared_ptr<server_credentials> _creds;
    server_socket _sock;
};

class tls_socket_impl : public net::socket_impl {
    shared_ptr<certificate_credentials> _cred;
    sstring _name;
    ::seastar::socket _socket;
public:
    tls_socket_impl(shared_ptr<certificate_credentials> cred, sstring name)
            : _cred(cred), _name(std::move(name)), _socket(make_socket()) {
    }
    virtual future<connected_socket> connect(socket_address sa, socket_address local, transport proto = transport::TCP) override {
        return _socket.connect(sa, local, proto).then([cred = std::move(_cred), name = std::move(_name)](connected_socket s) mutable {
            return wrap_client(cred, std::move(s), std::move(name));
        });
    }
    void set_reuseaddr(bool reuseaddr) override {
      _socket.set_reuseaddr(reuseaddr);
    }
    bool get_reuseaddr() const override {
      return _socket.get_reuseaddr();
    }
    virtual void shutdown() override {
        _socket.shutdown();
    }
};

}

data_source tls::tls_connected_socket_impl::source() {
    return data_source(std::make_unique<source_impl>(_session));
}

data_sink tls::tls_connected_socket_impl::sink() {
    return data_sink(std::make_unique<sink_impl>(_session));
}


future<connected_socket> tls::connect(shared_ptr<certificate_credentials> cred, socket_address sa, sstring name) {
    return engine().connect(sa).then([cred = std::move(cred), name = std::move(name)](connected_socket s) mutable {
        return wrap_client(cred, std::move(s), std::move(name));
    });
}

future<connected_socket> tls::connect(shared_ptr<certificate_credentials> cred, socket_address sa, socket_address local, sstring name) {
    return engine().connect(sa, local).then([cred = std::move(cred), name = std::move(name)](connected_socket s) mutable {
        return wrap_client(cred, std::move(s), std::move(name));
    });
}

socket tls::socket(shared_ptr<certificate_credentials> cred, sstring name) {
    return ::seastar::socket(std::make_unique<tls_socket_impl>(std::move(cred), std::move(name)));
}

future<connected_socket> tls::wrap_client(shared_ptr<certificate_credentials> cred, connected_socket&& s, sstring name) {
    session::session_ref sess(make_lw_shared<session>(session::type::CLIENT, std::move(cred), std::move(s), std::move(name)));
    connected_socket sock(std::make_unique<tls_connected_socket_impl>(std::move(sess)));
    return make_ready_future<connected_socket>(std::move(sock));
}

future<connected_socket> tls::wrap_server(shared_ptr<server_credentials> cred, connected_socket&& s) {
    session::session_ref sess(make_lw_shared<session>(session::type::SERVER, std::move(cred), std::move(s)));
    connected_socket sock(std::make_unique<tls_connected_socket_impl>(std::move(sess)));
    return make_ready_future<connected_socket>(std::move(sock));
}

server_socket tls::listen(shared_ptr<server_credentials> creds, socket_address sa, listen_options opts) {
    return listen(std::move(creds), seastar::listen(sa, opts));
}

server_socket tls::listen(shared_ptr<server_credentials> creds, server_socket ss) {
    server_socket ssls(std::make_unique<server_session>(creds, std::move(ss)));
    return server_socket(std::move(ssls));
}

}
#endif // TP_BUILD
