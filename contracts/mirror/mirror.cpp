#include <eosio/asset.hpp>
#include <eosio/eosio.hpp>
#include <eosio/transaction.hpp>

#include "../library/totems.hpp"
using namespace eosio;

CONTRACT mirror : public contract {
   public:
    using contract::contract;

    struct [[eosio::table]] Pairing {
        symbol_code synth_ticker;
        symbol_code base_ticker;
        asset base_locked;
        uint64_t primary_key() const { return synth_ticker.raw(); }
        uint64_t by_base() const { return base_ticker.raw(); }
    };

    typedef eosio::multi_index<"pairings"_n, Pairing,
        indexed_by<"bybase"_n, const_mem_fun<Pairing, uint64_t, &Pairing::by_base>>> pairings_table;

    [[eosio::action]]
    void setup(const symbol& synth_ticker, const symbol& base_ticker) {
        auto base_totem = totems::get_totem(base_ticker.code());
        check(base_totem.has_value(), "Base totem does not exist");
        auto synth_totem = totems::get_totem(synth_ticker.code());
        check(synth_totem.has_value(), "Synth totem does not exist");

        require_auth(base_totem->creator);
        check(base_totem->creator == synth_totem->creator, "Base and synth totems must have the same creator");
        check(synth_ticker.precision() == base_ticker.precision(), "Synth and base tickers must have the same precision");
        check(synth_ticker != base_ticker, "Synth and base tickers must be different");

        pairings_table pairings(get_self(), get_self().value);
        auto it = pairings.find(synth_ticker.code().raw());
        check(it == pairings.end(), "Pairing already exists for this synth ticker");

        pairings.emplace(get_self(), [&](auto& row) {
            row.synth_ticker = synth_ticker.code();
            row.base_ticker = base_ticker.code();
            row.base_locked = asset{0, base_ticker};
        });
    }

    [[eosio::action]]
    void mint(const name& mod, const name& minter, const asset& quantity, const asset& payment, const std::string& memo) {
        check(get_sender() == totems::TOTEMS_CONTRACT, "mint action can only be called by totems contract");
        totems::check_license(quantity.symbol.code(), get_self());
        check(payment.amount == 0, "Mirror mod does not accept payment");

        symbol synth_sym = quantity.symbol;

        pairings_table pairings(get_self(), get_self().value);
        auto pair_itr = pairings.find(synth_sym.code().raw());
        check(pair_itr != pairings.end(), "No pairing exists for this synth ticker");

        auto synth_totem = totems::get_totem(synth_sym.code());
        check(synth_totem.has_value(), "Synth totem does not exist");
        check(minter == synth_totem->creator, "Only the creator can mint synth tokens");

        symbol base_sym = symbol(pair_itr->base_ticker, synth_sym.precision());

        // Calculate how much base has been deposited but not yet tracked.
        // Sum all base_locked for the same base ticker to get total tracked,
        // then compare against actual balance.
        auto base_idx = pairings.get_index<"bybase"_n>();
        int64_t total_tracked = 0;
        for (auto it = base_idx.lower_bound(pair_itr->base_ticker.raw());
             it != base_idx.end() && it->base_ticker == pair_itr->base_ticker; ++it) {
            total_tracked += it->base_locked.amount;
        }

        asset actual_balance = totems::get_balance(get_self(), base_sym);
        int64_t delta = actual_balance.amount - total_tracked;
        check(delta > 0, "No new base tokens deposited for minting synths");

        pairings.modify(pair_itr, get_self(), [&](auto& row) {
            row.base_locked += asset{delta, base_sym};
        });

        totems::transfer(
            get_self(),
            minter,
            asset{delta, synth_sym},
            std::string("Minted synth tokens")
        );
    }

    [[eosio::on_notify(TOTEMS_MINT_NOTIFY)]]
    void on_mint(const name& mod, const name& minter, const asset& quantity, const asset& payment, const std::string& memo) {}

    [[eosio::on_notify(TOTEMS_TRANSFER_NOTIFY)]]
    void on_transfer(const name& from, const name& to, const asset& quantity, const std::string& memo) {
        if (to != get_self() || from == get_self()) {
            return;
        }

        symbol synth_sym = quantity.symbol;

        pairings_table pairings(get_self(), get_self().value);
        auto pair_itr = pairings.find(synth_sym.code().raw());
        if (pair_itr == pairings.end()) {
            return; // Not a synth token (e.g. base token deposit) — accept silently
        }

        totems::check_license(quantity.symbol.code(), get_self());

        symbol base_sym = symbol(pair_itr->base_ticker, synth_sym.precision());
        check(pair_itr->base_locked.amount >= quantity.amount, "Insufficient base reserves for redemption");

        pairings.modify(pair_itr, get_self(), [&](auto& row) {
            row.base_locked -= asset{quantity.amount, base_sym};
        });

        // Send base tokens to the redeemer
        totems::transfer(
            get_self(),
            from,
            asset{quantity.amount, base_sym},
            std::string("Redeemed synth tokens")
        );

        // Burn the synth tokens
        action(
            permission_level{get_self(), "active"_n},
            totems::TOTEMS_CONTRACT,
            "burn"_n,
            std::make_tuple(get_self(), quantity, std::string("Burned redeemed synths"))
        ).send();
    }
};
