# Resetting TTL parameters

Use the `table ttl reset` subcommand to disable [TTL](../../concepts/ttl.md) for the specified table.

General format of the command:

```bash
{{ ydb-cli }} [global options...] table ttl reset [options...] <table path>
```

* `global options`: [Global parameters](commands/global-options.md).
* `options`: [Parameters of the subcommand](#options).
* `table path`: The table path.

View a description of the TTL reset command:

```bash
{{ ydb-cli }} table ttl reset --help
```

## Examples {examples}

{% include [ydb-cli-profile](../../_includes/ydb-cli-profile.md) %}

Disable TTL for the `series` table:

```bash
{{ ydb-cli }} -p db1 table ttl reset \
  series
```
