#!/usr/bin/env python3
"""Create, inspect and destroy the Hetzner box that kei-node builds and runs on.

The box is the only environment in which the node has ever run, and it is billed
by the hour for as long as it exists, so the two operations that matter are
"make me one" and "make it stop costing money". Both are one command here; the
previous box was created by hand against the API and left nothing behind that
could recreate it.

Type defaults to "cx43,cx33" -- a preference order, not one type -- rather than
the cpx42 the first box used. Hetzner's 15 June 2026 adjustment raised CPX about
2.7x and CX only about 1.33x, which crossed the lines: cx43 is the same 8 vCPU
and 16 GB as cpx42 for about a quarter of the price, and cx33 is half the box
for an eighth of it (EUR 0.0160/h against EUR 0.1314/h).

The catch, and the reason --type takes a list and --wait-minutes exists: on the
CX line stock is the binding constraint, not price. On 2026-07-31 cx43, cx53 and
every CAX type were out of stock in every location, fsn1 had nothing at all, and
the cheapest 8 core box actually purchasable was the cpx42 itself. cx33 was in
stock, was bought, and was gone from every location twenty minutes later.

So waiting is a strategy rather than a fallback. A box that does not exist costs
nothing, which makes an hour of polling for cx33 strictly cheaper than an hour of
cpx42, and far cheaper than a week of whatever happened to be in stock at a bad
moment. Deleting a box gives up its slot, so do not delete one you still want.

A cold build is ~20 minutes on cx43 and ~40 on cx33; ccache makes incremental
builds seconds on either. The small box is why setup-build-box.sh adds swap and
derives its ninja job count from memory rather than cores.

Talks to api.hetzner.cloud with stdlib only: this runs from a Windows box with
no jq, no hcloud CLI and no administrator rights to install either.

Usage:
    python ci/provision-build-box.py create --wait-minutes 120
    python ci/provision-build-box.py status
    python ci/provision-build-box.py destroy --yes kei-build

The token comes from ~/.config/hcloud/token, or HCLOUD_TOKEN.
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

API = "https://api.hetzner.cloud/v1"
DEFAULT_NAME = "kei-build"
DEFAULT_TYPE = "cx43,cx33"
DEFAULT_LOCATION = "nbg1,hel1"
DEFAULT_IMAGE = "ubuntu-24.04"

SETUP_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "setup-build-box.sh")


class ApiError(Exception):
    pass


def call(token, path, method="GET", body=None):
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(API + path, data=data, method=method)
    request.add_header("Authorization", "Bearer " + token)
    request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            raw = response.read()
            return json.loads(raw) if raw else {}
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")
        # The token is the one thing in this script worth being careful about,
        # and Hetzner echoes nothing sensitive back, so the body is safe to show
        # and is usually the whole diagnosis.
        raise ApiError("{} {} -> HTTP {}: {}".format(method, path, error.code, detail))
    except urllib.error.URLError as error:
        raise ApiError("{} {} -> {}".format(method, path, error.reason))


def token_or_exit():
    """HCLOUD_TOKEN, or a file holding it.

    The file is the better of the two here. A token pasted into a shell command
    ends up in the shell history and in the transcript of whoever ran it; a
    token in a file gets read at the moment it is needed and never printed.
    """
    token = os.environ.get("HCLOUD_TOKEN", "").strip()
    if token:
        return token
    path = os.environ.get(
        "HCLOUD_TOKEN_FILE", os.path.join(os.path.expanduser("~"), ".config", "hcloud", "token")
    )
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as handle:
            token = handle.read().strip()
        if token:
            return token
    sys.exit(
        "No Hetzner token.\n"
        "Either put it in a file (preferred - it stays out of shell history):\n"
        "    {}\n"
        "or set HCLOUD_TOKEN in the environment.\n"
        "A new one: Hetzner Cloud console -> the project -> Security -> API tokens\n"
        "-> Generate, with Read & Write.".format(path)
    )


def server_type(token, name):
    for entry in call(token, "/server_types?per_page=50")["server_types"]:
        if entry["name"] == name:
            return entry
    raise ApiError("no server type named {}".format(name))


def price_at(entry, location):
    for price in entry.get("prices", []):
        if price["location"] == location:
            return price
    return None


def available_locations(token, entry):
    """Every location where this type can actually be created right now.

    Stock is the binding constraint on the cheap types, not price, and it moves
    on the order of minutes: on 2026-07-31 cx33 was in stock in nbg1, was bought,
    and was gone from every location twenty minutes later. `supported` is not the
    field to read here — cx43 is permanently supported in nbg1 and was buyable in
    none of them. Only `available` means yes.
    """
    return {
        datacenter["location"]["name"]
        for datacenter in call(token, "/datacenters")["datacenters"]
        if entry["id"] in datacenter["server_types"]["available"]
    }


def check_available(token, entry, location):
    """Refuse early, and say where the type *is* buyable, rather than letting the
    create fail with a resource_unavailable that reads like a bad request."""
    locations = available_locations(token, entry)
    if location in locations:
        return
    hint = ", ".join(sorted(locations)) or "nowhere"
    raise ApiError(
        "{} is not available in {} right now. Available in: {}.\n"
        "Pass --location, --type, or --wait-minutes to hold out for stock.".format(
            entry["name"], location, hint
        )
    )


def server_location(server):
    """Where a server is, for pricing.

    A server carries `location` directly; the older shape nested it under
    `datacenter`, which is what the docs still show in places. Take either, so
    reading a price never turns into a KeyError on an unrelated field.
    """
    if "location" in server:
        return server["location"]["name"]
    return server["datacenter"]["location"]["name"]


def find_server(token, name):
    servers = call(token, "/servers?name=" + name)["servers"]
    return servers[0] if servers else None


def resolve_ssh_keys(token, wanted):
    keys = call(token, "/ssh_keys?per_page=50")["ssh_keys"]
    names = [key["name"] for key in keys]
    if wanted:
        missing = [name for name in wanted if name not in names]
        if missing:
            raise ApiError(
                "no ssh key named {} in this project. Have: {}".format(
                    ", ".join(missing), ", ".join(names) or "none"
                )
            )
        return wanted
    if not keys:
        # A box with no key is reachable only by the emailed root password,
        # which is a worse place to end up than a clear failure here.
        raise ApiError(
            "this project has no SSH keys, so the new box would have no way in.\n"
            "Add one in the console (Security -> SSH keys) or pass --ssh-key."
        )
    return names


def wait_for_action(token, action, what):
    while action["status"] == "running":
        time.sleep(3)
        action = call(token, "/actions/{}".format(action["id"]))["action"]
    if action["status"] != "success":
        raise ApiError("{} failed: {}".format(what, json.dumps(action.get("error"))))


def pick_type(token, args):
    """The first --type that is actually in stock, waiting for one if asked.

    --type takes a preference list because the good types are exactly the ones
    that sell out, and which of them is buyable changes while you are deciding.
    Waiting is a real strategy rather than a fallback: a box that does not exist
    costs nothing, so holding out an hour for cx33 is cheaper than running a
    cpx42 for that hour, and much cheaper than running one for a week because it
    was what happened to be in stock.
    """
    wanted = [name.strip() for name in args.type.split(",") if name.strip()]
    locations = [name.strip() for name in args.location.split(",") if name.strip()]
    entries = [server_type(token, name) for name in wanted]
    deadline = time.time() + args.wait_minutes * 60
    announced = False
    while True:
        # Type first, then location: a cx43 in Helsinki beats a cx33 in
        # Nuremberg, and for this box the location is not otherwise meaningful.
        for entry in entries:
            in_stock = available_locations(token, entry)
            for location in locations:
                if location in in_stock:
                    return entry, location
        if time.time() >= deadline:
            if len(entries) == 1 and len(locations) == 1:
                check_available(token, entries[0], locations[0])
            raise ApiError(
                "none of {} are available in {} right now.\n"
                "Raise --wait-minutes, widen --location or --type.".format(
                    ", ".join(wanted), ", ".join(locations)
                )
            )
        if not announced:
            print(
                "none of {} in stock in {} yet; polling every 60s for {} minutes. "
                "Nothing is billing while this waits.".format(
                    ", ".join(wanted), ", ".join(locations), args.wait_minutes
                )
            )
            announced = True
        time.sleep(60)


def create(token, args):
    if find_server(token, args.name):
        sys.exit(
            "a server named {} already exists. `status` to see it, "
            "`destroy --yes {}` to remove it, or --name for a second one.".format(
                args.name, args.name
            )
        )

    entry, location = pick_type(token, args)
    price = price_at(entry, location)
    if price:
        print(
            "{}: {} vCPU, {} GB RAM, {} GB disk - EUR {:.4f}/h, EUR {:.2f}/mo (incl. VAT)".format(
                entry["name"],
                entry["cores"],
                entry["memory"],
                entry["disk"],
                float(price["price_hourly"]["gross"]),
                float(price["price_monthly"]["gross"]),
            )
        )

    with open(SETUP_SCRIPT, "r", encoding="utf-8") as handle:
        user_data = handle.read()
    if args.branch != "master":
        # cloud-init runs the script with no environment of ours, so a branch
        # override has to be baked in rather than exported.
        user_data = user_data.replace(
            'branch="${KEI_BRANCH:-master}"', 'branch="{}"'.format(args.branch), 1
        )

    keys = resolve_ssh_keys(token, args.ssh_key)
    print("creating {} ({}) in {} with ssh key(s): {}".format(
        args.name, entry["name"], location, ", ".join(keys)
    ))
    result = call(
        token,
        "/servers",
        method="POST",
        body={
            "name": args.name,
            "server_type": entry["name"],
            "image": args.image,
            "location": location,
            "ssh_keys": keys,
            "user_data": user_data,
            "labels": {"project": "kei-node", "role": "build"},
        },
    )
    server = result["server"]
    wait_for_action(token, result["action"], "server create")

    ipv4 = server["public_net"]["ipv4"]["ip"] if server["public_net"]["ipv4"] else None
    ipv6 = server["public_net"]["ipv6"]["ip"] if server["public_net"]["ipv6"] else None
    print("\n{} is up: ipv4 {}, ipv6 {}".format(args.name, ipv4 or "none", ipv6 or "none"))
    host = ipv4 or ipv6
    print(
        "\ncloud-init is now running ci/setup-build-box.sh on it. A cold build is\n"
        "~20 minutes, so nothing works yet. Watch it:\n"
        "    ssh root@{host} tail -f /var/log/kei-setup.log\n"
        "Ready when this says `ready`:\n"
        "    ssh root@{host} cat /root/kei-setup-status\n"
        "\nRPC is loopback-only by design, so reach it through the ssh session:\n"
        "    ssh root@{host}\n"
        "    curl -d '{{\"action\":\"version\"}}' http://[::ffff:127.0.0.1]:45000\n"
        "\nIt bills by the hour until it is deleted:\n"
        "    python ci/provision-build-box.py destroy --yes {name}".format(host=host, name=args.name)
    )


def status(token, args):
    servers = call(token, "/servers?per_page=50")["servers"]
    if not servers:
        print("no servers in this project - nothing is being billed.")
        return
    types = {entry["name"]: entry for entry in call(token, "/server_types?per_page=50")["server_types"]}
    total = 0.0
    for server in servers:
        entry = types.get(server["server_type"]["name"], {})
        price = price_at(entry, server_location(server)) if entry else None
        hourly = float(price["price_hourly"]["gross"]) if price else 0.0
        total += hourly
        ipv4 = server["public_net"]["ipv4"]["ip"] if server["public_net"]["ipv4"] else "-"
        print(
            "{:<16} {:<8} {:<8} {:<16} EUR {:.4f}/h  ({:.2f}/day)".format(
                server["name"],
                server["server_type"]["name"],
                server["status"],
                ipv4,
                hourly,
                hourly * 24,
            )
        )
    print("\ntotal: EUR {:.4f}/h - EUR {:.2f}/day if left running".format(total, total * 24))


def destroy(token, args):
    server = find_server(token, args.name)
    if not server:
        print("no server named {} — nothing to delete.".format(args.name))
        return
    if not args.yes:
        sys.exit(
            "refusing to delete {} ({}, {}) without --yes.\n"
            "Deleting is not reversible and the ccache goes with it - a fresh box "
            "is a ~20 minute cold build.".format(
                server["name"], server["server_type"]["name"], server["status"]
            )
        )
    result = call(token, "/servers/{}".format(server["id"]), method="DELETE")
    if result.get("action"):
        wait_for_action(token, result["action"], "server delete")
    print("deleted {}. It stops billing now.".format(args.name))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    new = sub.add_parser("create", help="create the build box and start its setup")
    new.add_argument("--name", default=DEFAULT_NAME)
    new.add_argument(
        "--type",
        default=DEFAULT_TYPE,
        help="comma-separated preference order, first one in stock wins (default: %(default)s)",
    )
    new.add_argument(
        "--wait-minutes",
        type=int,
        default=0,
        help="poll for stock this long instead of failing; a box that does not exist costs nothing",
    )
    new.add_argument("--location", default=DEFAULT_LOCATION)
    new.add_argument("--image", default=DEFAULT_IMAGE)
    new.add_argument("--branch", default="master", help="kei-node branch to build")
    new.add_argument("--ssh-key", action="append", default=[], help="repeatable; defaults to every key in the project")

    sub.add_parser("status", help="what exists and what it costs per hour")

    gone = sub.add_parser("destroy", help="delete a server so it stops billing")
    gone.add_argument("name", nargs="?", default=DEFAULT_NAME)
    gone.add_argument("--yes", action="store_true")

    args = parser.parse_args()
    token = token_or_exit()
    try:
        {"create": create, "status": status, "destroy": destroy}[args.command](token, args)
    except ApiError as error:
        sys.exit("hetzner: {}".format(error))


if __name__ == "__main__":
    main()
