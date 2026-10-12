module;

#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>

export module lemonsquash.lazy_resource;

export namespace Lemonsquash {
    template <typename Key, typename T>
    class LazyResourceCache;

    template <typename T>
    class LazyResource {
        struct State {
            std::function<T()> create;
            std::function<void(T)> release;
            std::mutex mutex;
            T value{};

            State(std::function<T()> allocate, std::function<void(T)> destroy)
                : create(std::move(allocate)), release(std::move(destroy)) {}

            ~State() noexcept {
                if (value != T{})
                    release(value);
            }

            T Get() {
                std::lock_guard lock(mutex);
                if (value == T{})
                    value = create();
                return value;
            }
        };

        std::shared_ptr<State> state;

        explicit LazyResource(std::shared_ptr<State> shared) : state(std::move(shared)) {}

        template <typename Key, typename Resource>
        friend class LazyResourceCache;

    public:
        LazyResource() = default;

        template <typename Create, typename Release>
        LazyResource(Create create, Release release)
            : state(std::make_shared<State>(std::move(create), std::move(release))) {
            static_assert(std::is_nothrow_invocable_v<Release&, T>, "Resource release must be noexcept");
        }

        T Get() const {
            return state ? state->Get() : T{};
        }

        explicit operator bool() const noexcept {
            return static_cast<bool>(state);
        }

        void Reset() noexcept {
            state.reset();
        }
    };

    template <typename Key, typename T>
    class LazyResourceCache {
        std::unordered_map<Key, std::weak_ptr<typename LazyResource<T>::State>> entries;
        std::mutex mutex;

    public:
        template <typename Create, typename Release>
        LazyResource<T> Acquire(const Key& key, Create create, Release release) {
            std::lock_guard lock(mutex);
            if (auto found = entries.find(key); found != entries.end()) {
                if (auto shared = found->second.lock())
                    return LazyResource<T>(std::move(shared));
            }

            std::erase_if(entries, [](const auto& entry) { return entry.second.expired(); });
            LazyResource<T> resource(std::move(create), std::move(release));
            entries.emplace(key, resource.state);
            return resource;
        }
    };
}
