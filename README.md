# mod-bot-economy

An [AzerothCore](https://www.azerothcore.org/) module for the
[Playerbots](https://github.com/liyunfan1223/mod-playerbots) fork (WotLK 3.3.5a) that gives
random playerbots a small, self-contained "Dad Mode" economy.

## What it does

- A gold bonus on creature kills.
- A warehouse ledger that records surplus deposits.
- A periodic, player-safe warehouse "sorter" that consolidates stacks.

Every behaviour is gated behind a single master switch, `DadMode.Enabled`, which is the
**canonical kill-switch for the whole "Dad Mode" module family** — it is declared here and
read by the sibling modules. With `DadMode.Enabled = 0` the module loads but does nothing.

## Configuration

See `conf/mod_bot_economy.conf.dist`. Key options:

| Key                                    | Description                              |
|----------------------------------------|------------------------------------------|
| `DadMode.Enabled`                      | Master switch for the Dad Mode family    |
| `DadMode.Economy.KillGoldBonusPct`     | Extra gold on kills (percent)            |
| `DadMode.Economy.SorterEnabled`        | Enable the warehouse sorter              |
| `DadMode.Economy.SorterIntervalSec`    | Sorter interval                          |
| `DadMode.Economy.WarehouseLootThreshold`| Quality threshold for warehouse deposits |

## Requirements

Built for the Playerbots fork of AzerothCore.

## Installation

Clone into your AzerothCore `modules/` directory and rebuild the worldserver.

## License

Released under the GNU GPL v2 (or later).
