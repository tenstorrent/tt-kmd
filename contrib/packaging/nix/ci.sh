#!/usr/bin/env bash

system=$(nix-instantiate --expr 'builtins.currentSystem' --eval --raw)
attrs=$(nix flake show --json | jq --arg system "$system" '.packages[$system] | keys | .[]' -r)
paths=()

for attr in $attrs; do
  paths+=(".#packages.$system.$attr")
done

nix build ${paths[@]}
