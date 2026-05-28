# Modular Architecture

The backend now has a small runtime module boundary under `server/app`.

## Server Runtime Contract

Every server capability that needs startup/shutdown wiring should implement
`ApplicationModule`:

- `configure()`: validate configuration and perform cheap setup before Drogon
  starts.
- `registerHandlers()`: bind callbacks and event subscriptions after shared
  infrastructure exists.
- `start()`: run async startup work after database/cache prerequisites are
  ready.
- `stop()`: release async resources during shutdown.

`ServerBootstrapper` owns the module list and the ordered startup stages. New
runtime features should be registered there instead of adding more concrete
startup code to `main.cpp`.

## Current Modules

- `Gb28181RuntimeModule`: GB28181 SIP/media runtime.
- `ProtocolRuntimeModule`: protocol dispatcher, protocol adapters, TCP/Agent
  ingress callbacks.
- `DomainEventRuntimeModule`: domain-event subscribers for links, WebSocket
  broadcasts, and open webhook dispatch.
- `AlertRuntimeModule`: alert engine and offline checker.
- `LinkRuntimeModule`: enabled link startup and link shutdown.

## Direction

Keep module code responsible for its own details, and keep orchestration code
responsible only for ordering. Controllers, services, domain objects, protocol
adapters, and event handlers should depend inward on shared abstractions rather
than calling across feature modules directly.

