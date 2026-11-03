-- Project Chronicles: complete PostgreSQL 16 schema for a fresh database.
-- Run tools/install_database.ps1, or apply this file with psql ON_ERROR_STOP=1.
-- One atomic installation; no historical upgrades, data backfills or rollback scripts.
-- Reapplying to the complete current schema preserves all existing data.

DO $chronicles_install$
DECLARE
    installed_version integer;
BEGIN
    PERFORM pg_advisory_xact_lock(7342091001);
    PERFORM set_config('search_path', 'pg_catalog, public', true);

    IF to_regclass('public.emulator_schema_version') IS NOT NULL THEN
        SELECT max(version) INTO installed_version FROM public.emulator_schema_version;
        IF installed_version = 44
           AND (SELECT count(*) FROM public.emulator_schema_version
                WHERE version BETWEEN 1 AND 44) = 44
           AND NOT EXISTS (
               SELECT 1 FROM unnest(ARRAY[
                   'public.emulator_accounts',
                   'public.emulator_agent_memories',
                   'public.emulator_character_achievements',
                   'public.emulator_character_adventure_objects',
                   'public.emulator_character_company',
                   'public.emulator_character_crafts',
                   'public.emulator_character_doodad_phases',
                   'public.emulator_character_emotions',
                   'public.emulator_character_fog_areas',
                   'public.emulator_character_guild_history',
                   'public.emulator_character_hotkeys',
                   'public.emulator_character_housing_items',
                   'public.emulator_character_items',
                   'public.emulator_character_job_boards',
                   'public.emulator_character_loot_first_kills',
                   'public.emulator_character_mates',
                   'public.emulator_character_perks',
                   'public.emulator_character_play_records',
                   'public.emulator_character_quests',
                   'public.emulator_character_shop_purchases',
                   'public.emulator_character_stackables',
                   'public.emulator_character_summons',
                   'public.emulator_character_trade_runs',
                   'public.emulator_character_trade_vouchers',
                   'public.emulator_character_utility_slots',
                   'public.emulator_characters',
                   'public.emulator_discord_accounts',
                   'public.emulator_friend_presence',
                   'public.emulator_friends',
                   'public.emulator_gathering_source_limits',
                   'public.emulator_gathering_sources',
                   'public.emulator_guild_members',
                   'public.emulator_guilds',
                   'public.emulator_housing_channels',
                   'public.emulator_housing_characters',
                   'public.emulator_housing_packages',
                   'public.emulator_housing_sites',
                   'public.emulator_housing_worlds',
                   'public.emulator_launcher_content',
                   'public.emulator_loot_claim_limits',
                   'public.emulator_loot_claim_receipts',
                   'public.emulator_mail',
                   'public.emulator_market',
                   'public.emulator_monster_reward_events',
                   'public.emulator_monster_reward_limits',
                   'public.emulator_monster_reward_receipts',
                   'public.emulator_monster_reward_server_owner',
                   'public.emulator_news',
                   'public.emulator_offline_admin_quarantine',
                   'public.emulator_quest_activity_remainders',
                   'public.emulator_quest_service_character_guards',
                   'public.emulator_quest_service_effect_limits',
                   'public.emulator_quest_service_effects',
                   'public.emulator_quest_service_guild_guards',
                   'public.emulator_ranking_records',
                   'public.emulator_reconnect_credentials',
                   'public.emulator_schema_version'
               ]) AS expected(table_name)
               WHERE to_regclass(expected.table_name) IS NULL
           ) THEN
            RETURN;
        END IF;
        RAISE EXCEPTION 'This installer requires an empty database or the complete schema 44; found schema %.', installed_version;
    END IF;

    IF EXISTS (
        SELECT 1 FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE n.nspname = 'public' AND c.relkind IN ('r', 'p', 'v', 'm', 'S', 'f')
    ) THEN
        RAISE EXCEPTION 'The public schema is not empty. Choose a dedicated empty database.';
    END IF;

    -- Final table definitions, including current defaults and integrity constraints.

    CREATE TABLE public.emulator_accounts (
        id bigint GENERATED ALWAYS AS IDENTITY NOT NULL,
        account_name text NOT NULL,
        password_salt text NOT NULL,
        password_hash text NOT NULL,
        password_iterations integer NOT NULL,
        created_at timestamp with time zone DEFAULT now() NOT NULL,
        account_role smallint DEFAULT 0 NOT NULL,
        banned boolean DEFAULT false NOT NULL,
        CONSTRAINT emulator_accounts_account_name_key UNIQUE (account_name),
        CONSTRAINT emulator_accounts_account_role_check CHECK ((account_role = ANY (ARRAY[0, 1]))),
        CONSTRAINT emulator_accounts_password_iterations_check CHECK ((password_iterations >= 100000)),
        CONSTRAINT emulator_accounts_pkey PRIMARY KEY (id)
    );

    CREATE TABLE public.emulator_agent_memories (
        id bigint GENERATED ALWAYS AS IDENTITY NOT NULL,
        agent_id text NOT NULL,
        description text NOT NULL,
        event_datetime text NOT NULL,
        priority smallint NOT NULL,
        reason text NOT NULL,
        CONSTRAINT emulator_agent_memories_agent_id_check CHECK (((octet_length(agent_id) >= 1) AND (octet_length(agent_id) <= 64))),
        CONSTRAINT emulator_agent_memories_description_check CHECK (((octet_length(description) >= 1) AND (octet_length(description) <= 2048))),
        CONSTRAINT emulator_agent_memories_event_datetime_check CHECK ((octet_length(event_datetime) <= 64)),
        CONSTRAINT emulator_agent_memories_pkey PRIMARY KEY (id),
        CONSTRAINT emulator_agent_memories_priority_check CHECK (((priority >= 1) AND (priority <= 10))),
        CONSTRAINT emulator_agent_memories_reason_check CHECK ((octet_length(reason) <= 512))
    );

    CREATE TABLE public.emulator_character_achievements (
        character_id bigint NOT NULL,
        achievement_cms_id integer NOT NULL,
        state integer NOT NULL,
        completion_tick bigint NOT NULL,
        CONSTRAINT emulator_character_achievements_achievement_cms_id_check CHECK ((achievement_cms_id > 0)),
        CONSTRAINT emulator_character_achievements_completion_tick_check CHECK ((completion_tick > 0)),
        CONSTRAINT emulator_character_achievements_pkey PRIMARY KEY (character_id, achievement_cms_id),
        CONSTRAINT emulator_character_achievements_state_check CHECK ((state = ANY (ARRAY[1, 2])))
    );

    CREATE TABLE public.emulator_character_adventure_objects (
        character_id bigint NOT NULL,
        object_cms_id integer NOT NULL,
        state smallint NOT NULL,
        CONSTRAINT emulator_character_adventure_objects_object_cms_id_check CHECK ((object_cms_id > 0)),
        CONSTRAINT emulator_character_adventure_objects_pkey PRIMARY KEY (character_id, object_cms_id),
        CONSTRAINT emulator_character_adventure_objects_state_check CHECK ((state = ANY (ARRAY[1, 2])))
    );

    CREATE TABLE public.emulator_character_company (
        character_id bigint NOT NULL,
        company_type smallint NOT NULL,
        contribution bigint DEFAULT 0 NOT NULL,
        join_epoch_ticks bigint DEFAULT 0 NOT NULL,
        activity_epoch_ticks bigint DEFAULT 0 NOT NULL,
        permit_expiry_epoch_ticks bigint DEFAULT 0 NOT NULL,
        unlocked_position_level integer DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_character_company_activity_epoch_ticks_check CHECK ((activity_epoch_ticks >= 0)),
        CONSTRAINT emulator_character_company_company_type_check CHECK (((company_type >= 0) AND (company_type <= 5))),
        CONSTRAINT emulator_character_company_contribution_check CHECK ((contribution >= 0)),
        CONSTRAINT emulator_character_company_join_epoch_ticks_check CHECK ((join_epoch_ticks >= 0)),
        CONSTRAINT emulator_character_company_permit_expiry_epoch_ticks_check CHECK ((permit_expiry_epoch_ticks >= 0)),
        CONSTRAINT emulator_character_company_pkey PRIMARY KEY (character_id),
        CONSTRAINT emulator_character_company_unlocked_position_level_check CHECK ((unlocked_position_level >= 0))
    );

    CREATE TABLE public.emulator_character_crafts (
        character_id bigint NOT NULL,
        craft_cms_id integer NOT NULL,
        CONSTRAINT emulator_character_crafts_craft_cms_id_check CHECK ((craft_cms_id > 0)),
        CONSTRAINT emulator_character_crafts_pkey PRIMARY KEY (character_id, craft_cms_id)
    );

    CREATE TABLE public.emulator_character_doodad_phases (
        character_id bigint NOT NULL,
        unit_id bigint NOT NULL,
        phase integer NOT NULL,
        CONSTRAINT emulator_character_doodad_phases_phase_check CHECK ((phase >= 0)),
        CONSTRAINT emulator_character_doodad_phases_pkey PRIMARY KEY (character_id, unit_id),
        CONSTRAINT emulator_character_doodad_phases_unit_id_check CHECK ((unit_id > 0))
    );

    CREATE TABLE public.emulator_character_emotions (
        character_id bigint NOT NULL,
        emotion_cms_id integer NOT NULL,
        CONSTRAINT emulator_character_emotions_emotion_cms_id_check CHECK ((emotion_cms_id > 0)),
        CONSTRAINT emulator_character_emotions_pkey PRIMARY KEY (character_id, emotion_cms_id)
    );

    CREATE TABLE public.emulator_character_fog_areas (
        character_id bigint NOT NULL,
        fog_area_id integer NOT NULL,
        CONSTRAINT emulator_character_fog_areas_fog_area_id_check CHECK ((fog_area_id > 0)),
        CONSTRAINT emulator_character_fog_areas_pkey PRIMARY KEY (character_id, fog_area_id)
    );

    CREATE TABLE public.emulator_character_guild_history (
        character_id bigint NOT NULL,
        last_leave_epoch_ticks bigint DEFAULT 0 NOT NULL,
        last_disband_epoch_ticks bigint DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_character_guild_history_last_disband_epoch_ticks_check CHECK ((last_disband_epoch_ticks >= 0)),
        CONSTRAINT emulator_character_guild_history_last_leave_epoch_ticks_check CHECK ((last_leave_epoch_ticks >= 0)),
        CONSTRAINT emulator_character_guild_history_pkey PRIMARY KEY (character_id)
    );

    CREATE TABLE public.emulator_character_hotkeys (
        character_id bigint NOT NULL,
        slot smallint NOT NULL,
        kind smallint NOT NULL,
        value integer NOT NULL,
        CONSTRAINT emulator_character_hotkeys_kind_check CHECK (((kind >= 1) AND (kind <= 4))),
        CONSTRAINT emulator_character_hotkeys_pkey PRIMARY KEY (character_id, slot),
        CONSTRAINT emulator_character_hotkeys_slot_check CHECK (((slot >= 0) AND (slot <= 63))),
        CONSTRAINT emulator_character_hotkeys_value_check CHECK ((value > 0))
    );

    CREATE TABLE public.emulator_character_housing_items (
        character_id bigint NOT NULL,
        item_cms_id integer NOT NULL,
        quantity integer NOT NULL,
        installed_count integer DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_character_housing_items_check CHECK ((installed_count <= quantity)),
        CONSTRAINT emulator_character_housing_items_installed_count_check CHECK ((installed_count >= 0)),
        CONSTRAINT emulator_character_housing_items_item_cms_id_check CHECK ((item_cms_id > 0)),
        CONSTRAINT emulator_character_housing_items_pkey PRIMARY KEY (character_id, item_cms_id),
        CONSTRAINT emulator_character_housing_items_quantity_check CHECK ((quantity >= 0))
    );

    CREATE TABLE public.emulator_character_items (
        id bigint GENERATED ALWAYS AS IDENTITY NOT NULL,
        character_id bigint NOT NULL,
        item_cms_id integer NOT NULL,
        inventory_slot integer NOT NULL,
        equip_slot integer,
        quantity integer DEFAULT 1 NOT NULL,
        durability integer DEFAULT 10000 NOT NULL,
        grade integer DEFAULT 0 NOT NULL,
        created_at timestamp with time zone DEFAULT now() NOT NULL,
        created_epoch_ticks bigint DEFAULT 0 NOT NULL,
        polish_step integer DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_character_items_character_id_inventory_slot_key UNIQUE (character_id, inventory_slot),
        CONSTRAINT emulator_character_items_created_epoch_ticks_check CHECK ((created_epoch_ticks >= 0)),
        CONSTRAINT emulator_character_items_durability_check CHECK ((durability >= 0)),
        CONSTRAINT emulator_character_items_equip_slot_check CHECK (((equip_slot >= 0) AND (equip_slot <= 24))),
        CONSTRAINT emulator_character_items_grade_check CHECK (((grade >= 0) AND (grade <= 255))),
        CONSTRAINT emulator_character_items_inventory_slot_check CHECK (((inventory_slot >= 0) AND (inventory_slot <= 999))),
        CONSTRAINT emulator_character_items_item_cms_id_check CHECK ((item_cms_id > 0)),
        CONSTRAINT emulator_character_items_pkey PRIMARY KEY (id),
        CONSTRAINT emulator_character_items_polish_step_range CHECK (((polish_step >= 0) AND (polish_step <= 15))),
        CONSTRAINT emulator_character_items_quantity_check CHECK ((quantity > 0))
    );

    CREATE TABLE public.emulator_character_job_boards (
        character_id bigint NOT NULL,
        board_cms_id integer NOT NULL,
        period bigint NOT NULL,
        refreshes integer NOT NULL,
        CONSTRAINT emulator_character_job_boards_board_cms_id_check CHECK ((board_cms_id > 0)),
        CONSTRAINT emulator_character_job_boards_period_check CHECK ((period >= 0)),
        CONSTRAINT emulator_character_job_boards_pkey PRIMARY KEY (character_id, board_cms_id),
        CONSTRAINT emulator_character_job_boards_refreshes_check CHECK ((refreshes >= 0))
    );

    CREATE TABLE public.emulator_character_loot_first_kills (
        character_id bigint NOT NULL,
        npc_cms_id integer NOT NULL,
        CONSTRAINT emulator_character_loot_first_kills_npc_cms_id_check CHECK ((npc_cms_id > 0)),
        CONSTRAINT emulator_character_loot_first_kills_pkey PRIMARY KEY (character_id, npc_cms_id)
    );

    CREATE TABLE public.emulator_character_mates (
        character_id bigint NOT NULL,
        mate_cms_id integer NOT NULL,
        state smallint NOT NULL,
        housing_lot_number integer DEFAULT 0 NOT NULL,
        intimacy integer DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_character_mates_housing_lot_number_check CHECK ((housing_lot_number >= 0)),
        CONSTRAINT emulator_character_mates_intimacy_check CHECK ((intimacy >= 0)),
        CONSTRAINT emulator_character_mates_mate_cms_id_check CHECK ((mate_cms_id > 0)),
        CONSTRAINT emulator_character_mates_pkey PRIMARY KEY (character_id, mate_cms_id),
        CONSTRAINT emulator_character_mates_state_check CHECK (((state >= 1) AND (state <= 4)))
    );

    CREATE TABLE public.emulator_character_perks (
        character_id bigint NOT NULL,
        perk_cms_id integer NOT NULL,
        perk_level integer NOT NULL,
        CONSTRAINT emulator_character_perks_perk_cms_id_check CHECK ((perk_cms_id > 0)),
        CONSTRAINT emulator_character_perks_perk_level_check CHECK ((perk_level >= 0)),
        CONSTRAINT emulator_character_perks_pkey PRIMARY KEY (character_id, perk_cms_id)
    );

    CREATE TABLE public.emulator_character_play_records (
        character_id bigint NOT NULL,
        play_record_cms_id integer NOT NULL,
        count bigint NOT NULL,
        CONSTRAINT emulator_character_play_records_count_check CHECK ((count >= 0)),
        CONSTRAINT emulator_character_play_records_pkey PRIMARY KEY (character_id, play_record_cms_id),
        CONSTRAINT emulator_character_play_records_play_record_cms_id_check CHECK ((play_record_cms_id > 0))
    );

    CREATE TABLE public.emulator_character_quests (
        character_id bigint NOT NULL,
        quest_cms_id integer NOT NULL,
        complete boolean DEFAULT false NOT NULL,
        success_count integer DEFAULT 0 NOT NULL,
        objectives jsonb DEFAULT '{}'::jsonb NOT NULL,
        credited_kills jsonb DEFAULT '[]'::jsonb NOT NULL,
        attempt integer DEFAULT 0 NOT NULL,
        abandoned boolean DEFAULT false NOT NULL,
        started_epoch_ms bigint DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_character_quests_attempt_check CHECK ((attempt >= 0)),
        CONSTRAINT emulator_character_quests_credited_kills_check CHECK ((jsonb_typeof(credited_kills) = 'array'::text)),
        CONSTRAINT emulator_character_quests_objectives_check CHECK ((jsonb_typeof(objectives) = 'object'::text)),
        CONSTRAINT emulator_character_quests_pkey PRIMARY KEY (character_id, quest_cms_id),
        CONSTRAINT emulator_character_quests_quest_cms_id_check CHECK ((quest_cms_id > 0)),
        CONSTRAINT emulator_character_quests_started_epoch_ms_check CHECK ((started_epoch_ms >= 0)),
        CONSTRAINT emulator_character_quests_success_count_check CHECK ((success_count >= 0))
    );

    CREATE TABLE public.emulator_character_shop_purchases (
        character_id bigint NOT NULL,
        shop_id integer NOT NULL,
        item_cms_id integer NOT NULL,
        purchased_count integer NOT NULL,
        reset_epoch_ticks bigint DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_character_shop_purchases_item_cms_id_check CHECK ((item_cms_id > 0)),
        CONSTRAINT emulator_character_shop_purchases_pkey PRIMARY KEY (character_id, shop_id, item_cms_id),
        CONSTRAINT emulator_character_shop_purchases_purchased_count_check CHECK ((purchased_count >= 0)),
        CONSTRAINT emulator_character_shop_purchases_reset_epoch_ticks_check CHECK ((reset_epoch_ticks >= 0)),
        CONSTRAINT emulator_character_shop_purchases_shop_id_check CHECK ((shop_id > 0))
    );

    CREATE TABLE public.emulator_character_stackables (
        character_id bigint NOT NULL,
        item_cms_id integer NOT NULL,
        quantity integer NOT NULL,
        CONSTRAINT emulator_character_stackables_item_cms_id_check CHECK ((item_cms_id > 0)),
        CONSTRAINT emulator_character_stackables_pkey PRIMARY KEY (character_id, item_cms_id),
        CONSTRAINT emulator_character_stackables_quantity_check CHECK ((quantity >= 0))
    );

    CREATE TABLE public.emulator_character_summons (
        character_id bigint NOT NULL,
        summon_cms_id integer NOT NULL,
        CONSTRAINT emulator_character_summons_pkey PRIMARY KEY (character_id, summon_cms_id),
        CONSTRAINT emulator_character_summons_summon_cms_id_check CHECK ((summon_cms_id > 0))
    );

    CREATE TABLE public.emulator_character_trade_runs (
        character_id bigint NOT NULL,
        phase smallint NOT NULL,
        start_post_id integer DEFAULT 0 NOT NULL,
        parked_post_id integer DEFAULT 0 NOT NULL,
        vehicle_cms_id integer DEFAULT 0 NOT NULL,
        started_epoch_ticks bigint DEFAULT 0 NOT NULL,
        goods jsonb DEFAULT '[]'::jsonb NOT NULL,
        wagon_out boolean DEFAULT false NOT NULL,
        wagon_map_id integer DEFAULT 0 NOT NULL,
        wagon_x integer DEFAULT 0 NOT NULL,
        wagon_y integer DEFAULT 0 NOT NULL,
        wagon_z integer DEFAULT 0 NOT NULL,
        wagon_yaw real DEFAULT 0 NOT NULL,
        wagon_health integer DEFAULT 0 NOT NULL,
        last_wagon_spawn_epoch_ticks bigint DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_character_trade_run_last_wagon_spawn_epoch_ticks_check CHECK ((last_wagon_spawn_epoch_ticks >= 0)),
        CONSTRAINT emulator_character_trade_runs_parked_post_id_check CHECK ((parked_post_id >= 0)),
        CONSTRAINT emulator_character_trade_runs_phase_check CHECK (((phase >= 0) AND (phase <= 4))),
        CONSTRAINT emulator_character_trade_runs_pkey PRIMARY KEY (character_id),
        CONSTRAINT emulator_character_trade_runs_start_post_id_check CHECK ((start_post_id >= 0)),
        CONSTRAINT emulator_character_trade_runs_started_epoch_ticks_check CHECK ((started_epoch_ticks >= 0)),
        CONSTRAINT emulator_character_trade_runs_vehicle_cms_id_check CHECK ((vehicle_cms_id >= 0)),
        CONSTRAINT emulator_character_trade_runs_wagon_health_check CHECK ((wagon_health >= 0))
    );

    CREATE TABLE public.emulator_character_trade_vouchers (
        character_id bigint NOT NULL,
        voucher_id bigint NOT NULL,
        good_cms_id integer NOT NULL,
        created_epoch_ticks bigint NOT NULL,
        expiration_epoch_ticks bigint NOT NULL,
        redeemed boolean DEFAULT false NOT NULL,
        CONSTRAINT emulator_character_trade_vouchers_check CHECK ((expiration_epoch_ticks >= created_epoch_ticks)),
        CONSTRAINT emulator_character_trade_vouchers_created_epoch_ticks_check CHECK ((created_epoch_ticks >= 0)),
        CONSTRAINT emulator_character_trade_vouchers_good_cms_id_check CHECK ((good_cms_id > 0)),
        CONSTRAINT emulator_character_trade_vouchers_pkey PRIMARY KEY (character_id, voucher_id),
        CONSTRAINT emulator_character_trade_vouchers_voucher_id_check CHECK ((voucher_id > 0))
    );

    CREATE TABLE public.emulator_character_utility_slots (
        character_id bigint NOT NULL,
        slot smallint NOT NULL,
        item_cms_id integer NOT NULL,
        quantity integer NOT NULL,
        CONSTRAINT emulator_character_utility_slots_item_cms_id_check CHECK ((item_cms_id >= 0)),
        CONSTRAINT emulator_character_utility_slots_pkey PRIMARY KEY (character_id, slot),
        CONSTRAINT emulator_character_utility_slots_quantity_check CHECK ((quantity >= 0)),
        CONSTRAINT emulator_character_utility_slots_slot_check CHECK (((slot >= 0) AND (slot <= 3)))
    );

    CREATE TABLE public.emulator_characters (
        id bigint GENERATED ALWAYS AS IDENTITY NOT NULL,
        account_id bigint NOT NULL,
        world_id integer NOT NULL,
        lobby_slot integer NOT NULL,
        name text NOT NULL,
        class_id integer NOT NULL,
        level integer DEFAULT 1 NOT NULL,
        customization_hex text DEFAULT ''::text NOT NULL,
        created_at timestamp with time zone DEFAULT now() NOT NULL,
        world_map_id integer,
        world_pos_x_mm integer,
        world_pos_y_mm integer,
        world_pos_z_mm integer,
        world_yaw real,
        experience bigint DEFAULT 0 NOT NULL,
        world_position_saved_at timestamp with time zone,
        camera_yaw real,
        camera_pitch real,
        starter_gear_version integer DEFAULT 0 NOT NULL,
        gameplay_revision bigint DEFAULT 0 NOT NULL,
        gold bigint DEFAULT 0 NOT NULL,
        player_dead boolean DEFAULT false NOT NULL,
        hersander_token bigint DEFAULT 0 NOT NULL,
        level_up_points integer DEFAULT 0 NOT NULL,
        weapon_points integer DEFAULT 0 NOT NULL,
        con_bonus integer DEFAULT 0 NOT NULL,
        dex_bonus integer DEFAULT 0 NOT NULL,
        end_bonus integer DEFAULT 0 NOT NULL,
        int_bonus integer DEFAULT 0 NOT NULL,
        str_bonus integer DEFAULT 0 NOT NULL,
        tal_bonus integer DEFAULT 0 NOT NULL,
        nui_blessing_level integer DEFAULT 0 NOT NULL,
        dedicated_nui_tears integer DEFAULT 0 NOT NULL,
        active_weapon_slot smallint DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_characters_account_id_world_id_lobby_slot_key UNIQUE (account_id, world_id, lobby_slot),
        CONSTRAINT emulator_characters_active_weapon_slot_check CHECK (((active_weapon_slot >= 0) AND (active_weapon_slot <= 1))),
        CONSTRAINT emulator_characters_class_id_check CHECK (((class_id >= 1) AND (class_id <= 11))),
        CONSTRAINT emulator_characters_con_bonus_check CHECK (((con_bonus >= 0) AND (con_bonus <= 1000))),
        CONSTRAINT emulator_characters_dedicated_nui_tears_check CHECK (((dedicated_nui_tears >= 0) AND (dedicated_nui_tears <= 100000))),
        CONSTRAINT emulator_characters_dex_bonus_check CHECK (((dex_bonus >= 0) AND (dex_bonus <= 1000))),
        CONSTRAINT emulator_characters_end_bonus_check CHECK (((end_bonus >= 0) AND (end_bonus <= 1000))),
        CONSTRAINT emulator_characters_gameplay_revision_check CHECK ((gameplay_revision >= 0)),
        CONSTRAINT emulator_characters_gold_check CHECK ((gold >= 0)),
        CONSTRAINT emulator_characters_gold_limit CHECK ((gold <= '9000000000000000'::bigint)),
        CONSTRAINT emulator_characters_hersander_token_check CHECK (((hersander_token >= 0) AND (hersander_token <= '9000000000000000'::bigint))),
        CONSTRAINT emulator_characters_int_bonus_check CHECK (((int_bonus >= 0) AND (int_bonus <= 1000))),
        CONSTRAINT emulator_characters_level_up_points_check CHECK (((level_up_points >= 0) AND (level_up_points <= 100000))),
        CONSTRAINT emulator_characters_lobby_slot_check CHECK (((lobby_slot >= 0) AND (lobby_slot <= 3))),
        CONSTRAINT emulator_characters_nui_blessing_level_check CHECK (((nui_blessing_level >= 0) AND (nui_blessing_level <= 30))),
        CONSTRAINT emulator_characters_pkey PRIMARY KEY (id),
        CONSTRAINT emulator_characters_str_bonus_check CHECK (((str_bonus >= 0) AND (str_bonus <= 1000))),
        CONSTRAINT emulator_characters_tal_bonus_check CHECK (((tal_bonus >= 0) AND (tal_bonus <= 1000))),
        CONSTRAINT emulator_characters_weapon_points_check CHECK (((weapon_points >= 0) AND (weapon_points <= 100000)))
    );

    CREATE TABLE public.emulator_discord_accounts (
        discord_user_id text NOT NULL,
        account_id bigint NOT NULL,
        linked_at timestamp with time zone DEFAULT now() NOT NULL,
        CONSTRAINT emulator_discord_accounts_account_id_key UNIQUE (account_id),
        CONSTRAINT emulator_discord_accounts_discord_user_id_check CHECK ((discord_user_id ~ '^[1-9][0-9]{16,19}$'::text)),
        CONSTRAINT emulator_discord_accounts_pkey PRIMARY KEY (discord_user_id)
    );

    CREATE TABLE public.emulator_friend_presence (
        world_id integer NOT NULL,
        account_id bigint NOT NULL,
        character_id bigint NOT NULL,
        last_login_epoch_ticks bigint NOT NULL,
        CONSTRAINT emulator_friend_presence_character_id_check CHECK ((character_id > 0)),
        CONSTRAINT emulator_friend_presence_last_login_epoch_ticks_check CHECK ((last_login_epoch_ticks >= 0)),
        CONSTRAINT emulator_friend_presence_pkey PRIMARY KEY (world_id, account_id),
        CONSTRAINT emulator_friend_presence_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_friends (
        world_id integer NOT NULL,
        owner_account_id bigint NOT NULL,
        friend_account_id bigint NOT NULL,
        alias text DEFAULT ''::text NOT NULL,
        created_epoch_ticks bigint NOT NULL,
        recommend_dismissed boolean DEFAULT false NOT NULL,
        CONSTRAINT emulator_friends_alias_check CHECK ((octet_length(alias) <= 64)),
        CONSTRAINT emulator_friends_check CHECK ((owner_account_id <> friend_account_id)),
        CONSTRAINT emulator_friends_created_epoch_ticks_check CHECK ((created_epoch_ticks > 0)),
        CONSTRAINT emulator_friends_pkey PRIMARY KEY (world_id, owner_account_id, friend_account_id),
        CONSTRAINT emulator_friends_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_gathering_source_limits (
        world_id integer NOT NULL,
        source_count bigint NOT NULL,
        CONSTRAINT emulator_gathering_source_limits_pkey PRIMARY KEY (world_id),
        CONSTRAINT emulator_gathering_source_limits_source_count_check CHECK (((source_count >= 0) AND (source_count <= 1000000))),
        CONSTRAINT emulator_gathering_source_limits_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_gathering_sources (
        world_id integer NOT NULL,
        map_id integer NOT NULL,
        source_kind smallint NOT NULL,
        sector_id bigint NOT NULL,
        local_index bigint NOT NULL,
        generation bigint NOT NULL,
        active boolean NOT NULL,
        phase integer,
        next_transition_epoch_ticks bigint,
        timed_successor_phase integer,
        pending_receipt_id text,
        next_transition_delay_ticks bigint,
        CONSTRAINT emulator_gathering_sources_check CHECK ((((pending_receipt_id IS NULL) AND (next_transition_delay_ticks IS NULL)) OR ((pending_receipt_id IS NOT NULL) AND (next_transition_epoch_ticks IS NULL)))),
        CONSTRAINT emulator_gathering_sources_generation_check CHECK ((generation >= 0)),
        CONSTRAINT emulator_gathering_sources_local_index_check CHECK (((local_index >= 0) AND (local_index <= '4294967295'::bigint))),
        CONSTRAINT emulator_gathering_sources_map_id_check CHECK ((map_id > 0)),
        CONSTRAINT emulator_gathering_sources_next_transition_delay_ticks_check CHECK (((next_transition_delay_ticks IS NULL) OR (next_transition_delay_ticks > 0))),
        CONSTRAINT emulator_gathering_sources_next_transition_epoch_ticks_check CHECK (((next_transition_epoch_ticks IS NULL) OR (next_transition_epoch_ticks > 0))),
        CONSTRAINT emulator_gathering_sources_pending_receipt_id_check CHECK (((pending_receipt_id IS NULL) OR (pending_receipt_id ~ '^[0-9a-f]{32}$'::text))),
        CONSTRAINT emulator_gathering_sources_phase_check CHECK (((phase IS NULL) OR (phase >= 0))),
        CONSTRAINT emulator_gathering_sources_pkey PRIMARY KEY (world_id, map_id, source_kind, sector_id, local_index),
        CONSTRAINT emulator_gathering_sources_sector_id_check CHECK (((sector_id >= 0) AND (sector_id <= '4294967295'::bigint))),
        CONSTRAINT emulator_gathering_sources_source_kind_check CHECK ((source_kind = ANY (ARRAY[0, 1]))),
        CONSTRAINT emulator_gathering_sources_timed_successor_phase_check CHECK (((timed_successor_phase IS NULL) OR (timed_successor_phase >= 0))),
        CONSTRAINT emulator_gathering_sources_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_guild_members (
        character_id bigint NOT NULL,
        guild_id bigint NOT NULL,
        member_grade smallint NOT NULL,
        contribution integer DEFAULT 0 NOT NULL,
        joined_epoch_ticks bigint NOT NULL,
        last_connect_epoch_ticks bigint DEFAULT 0 NOT NULL,
        company_position integer DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_guild_members_contribution_check CHECK ((contribution >= 0)),
        CONSTRAINT emulator_guild_members_joined_epoch_ticks_check CHECK ((joined_epoch_ticks >= 0)),
        CONSTRAINT emulator_guild_members_last_connect_epoch_ticks_check CHECK ((last_connect_epoch_ticks >= 0)),
        CONSTRAINT emulator_guild_members_member_grade_check CHECK ((member_grade = ANY (ARRAY[1, 2, 3]))),
        CONSTRAINT emulator_guild_members_pkey PRIMARY KEY (character_id)
    );

    CREATE TABLE public.emulator_guilds (
        id bigint GENERATED ALWAYS AS IDENTITY NOT NULL,
        world_id integer NOT NULL,
        name text NOT NULL,
        description text DEFAULT ''::text NOT NULL,
        notice text DEFAULT ''::text NOT NULL,
        notice_character_id bigint DEFAULT 0 NOT NULL,
        notice_epoch_ticks bigint DEFAULT 0 NOT NULL,
        join_type smallint NOT NULL,
        join_level integer NOT NULL,
        join_company_position integer NOT NULL,
        experience integer DEFAULT 0 NOT NULL,
        guild_level integer DEFAULT 1 NOT NULL,
        created_epoch_ticks bigint NOT NULL,
        join_company_type smallint DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_guilds_created_epoch_ticks_check CHECK ((created_epoch_ticks >= 0)),
        CONSTRAINT emulator_guilds_experience_check CHECK ((experience >= 0)),
        CONSTRAINT emulator_guilds_guild_level_check CHECK ((guild_level >= 1)),
        CONSTRAINT emulator_guilds_join_company_position_check CHECK ((join_company_position >= 0)),
        CONSTRAINT emulator_guilds_join_level_check CHECK ((join_level >= 0)),
        CONSTRAINT emulator_guilds_join_type_check CHECK ((join_type = ANY (ARRAY[0, 1]))),
        CONSTRAINT emulator_guilds_name_check CHECK (((char_length(name) >= 1) AND (char_length(name) <= 64))),
        CONSTRAINT emulator_guilds_notice_epoch_ticks_check CHECK ((notice_epoch_ticks >= 0)),
        CONSTRAINT emulator_guilds_pkey PRIMARY KEY (id)
    );

    CREATE TABLE public.emulator_housing_channels (
        world_id integer NOT NULL,
        zone_key integer NOT NULL,
        channel integer NOT NULL,
        version bigint NOT NULL,
        guild_id bigint DEFAULT 0 NOT NULL,
        guild_name text DEFAULT ''::text NOT NULL,
        area_name text DEFAULT ''::text NOT NULL,
        contract_type smallint NOT NULL,
        expiry_epoch_ticks bigint DEFAULT 0 NOT NULL,
        residents jsonb DEFAULT '[]'::jsonb NOT NULL,
        CONSTRAINT emulator_housing_channels_channel_check CHECK ((channel > 0)),
        CONSTRAINT emulator_housing_channels_contract_type_check CHECK (((contract_type >= 0) AND (contract_type <= 3))),
        CONSTRAINT emulator_housing_channels_guild_id_check CHECK ((guild_id >= 0)),
        CONSTRAINT emulator_housing_channels_pkey PRIMARY KEY (world_id, zone_key, channel),
        CONSTRAINT emulator_housing_channels_version_check CHECK ((version > 0)),
        CONSTRAINT emulator_housing_channels_zone_key_check CHECK ((zone_key > 0))
    );

    CREATE TABLE public.emulator_housing_characters (
        world_id integer NOT NULL,
        character_id bigint NOT NULL,
        version bigint NOT NULL,
        unlocked_site_types jsonb DEFAULT '[]'::jsonb NOT NULL,
        guild_unlocked boolean DEFAULT false NOT NULL,
        CONSTRAINT emulator_housing_characters_pkey PRIMARY KEY (world_id, character_id),
        CONSTRAINT emulator_housing_characters_version_check CHECK ((version > 0))
    );

    CREATE TABLE public.emulator_housing_packages (
        world_id integer NOT NULL,
        package_id bigint NOT NULL,
        version bigint NOT NULL,
        owner_character_id bigint NOT NULL,
        site_type smallint NOT NULL,
        created_epoch_ticks bigint NOT NULL,
        zone_key integer NOT NULL,
        original_lot_number integer NOT NULL,
        original_channel integer NOT NULL,
        reason smallint NOT NULL,
        contents jsonb NOT NULL,
        CONSTRAINT emulator_housing_packages_package_id_check CHECK ((package_id > 0)),
        CONSTRAINT emulator_housing_packages_pkey PRIMARY KEY (world_id, package_id),
        CONSTRAINT emulator_housing_packages_reason_check CHECK (((reason >= 0) AND (reason <= 3))),
        CONSTRAINT emulator_housing_packages_site_type_check CHECK (((site_type >= 0) AND (site_type <= 3))),
        CONSTRAINT emulator_housing_packages_version_check CHECK ((version > 0))
    );

    CREATE TABLE public.emulator_housing_sites (
        world_id integer NOT NULL,
        zone_key integer NOT NULL,
        channel integer NOT NULL,
        lot_number integer NOT NULL,
        version bigint NOT NULL,
        authority smallint NOT NULL,
        owner_character_id bigint NOT NULL,
        owner_account_id bigint NOT NULL,
        owner_name text NOT NULL,
        guild_id bigint DEFAULT 0 NOT NULL,
        contract_type smallint NOT NULL,
        expiry_epoch_ticks bigint NOT NULL,
        contracted_epoch_ticks bigint NOT NULL,
        return_position jsonb,
        contents jsonb NOT NULL,
        edit_save_cooldown_start_ticks bigint DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_housing_sites_authority_check CHECK (((authority >= 1) AND (authority <= 2))),
        CONSTRAINT emulator_housing_sites_channel_check CHECK ((channel > 0)),
        CONSTRAINT emulator_housing_sites_contract_type_check CHECK (((contract_type >= 0) AND (contract_type <= 3))),
        CONSTRAINT emulator_housing_sites_guild_id_check CHECK ((guild_id >= 0)),
        CONSTRAINT emulator_housing_sites_lot_number_check CHECK ((lot_number > 0)),
        CONSTRAINT emulator_housing_sites_pkey PRIMARY KEY (world_id, zone_key, channel, lot_number),
        CONSTRAINT emulator_housing_sites_version_check CHECK ((version > 0)),
        CONSTRAINT emulator_housing_sites_zone_key_check CHECK ((zone_key > 0))
    );

    CREATE TABLE public.emulator_housing_worlds (
        world_id integer NOT NULL,
        next_package_id bigint NOT NULL,
        CONSTRAINT emulator_housing_worlds_next_package_id_check CHECK ((next_package_id > 0)),
        CONSTRAINT emulator_housing_worlds_pkey PRIMARY KEY (world_id)
    );

    CREATE TABLE public.emulator_launcher_content (
        kind text NOT NULL,
        id text NOT NULL,
        document jsonb NOT NULL,
        published boolean DEFAULT false NOT NULL,
        sort_order integer DEFAULT 0 NOT NULL,
        updated_at bigint NOT NULL,
        CONSTRAINT emulator_launcher_content_document_check CHECK ((jsonb_typeof(document) = 'object'::text)),
        CONSTRAINT emulator_launcher_content_id_check CHECK ((id ~ '^[a-z0-9-]{1,80}$'::text)),
        CONSTRAINT emulator_launcher_content_kind_check CHECK ((kind = ANY (ARRAY['store'::text, 'community'::text]))),
        CONSTRAINT emulator_launcher_content_pkey PRIMARY KEY (kind, id),
        CONSTRAINT emulator_launcher_content_sort_order_check CHECK (((sort_order >= '-1000'::integer) AND (sort_order <= 1000))),
        CONSTRAINT emulator_launcher_content_updated_at_check CHECK ((updated_at > 0))
    );

    CREATE TABLE public.emulator_loot_claim_limits (
        world_id integer NOT NULL,
        receipt_count bigint NOT NULL,
        CONSTRAINT emulator_loot_claim_limits_pkey PRIMARY KEY (world_id),
        CONSTRAINT emulator_loot_claim_limits_receipt_count_check CHECK (((receipt_count >= 0) AND (receipt_count <= 1000000))),
        CONSTRAINT emulator_loot_claim_limits_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_loot_claim_receipts (
        receipt_id text NOT NULL,
        account_id bigint NOT NULL,
        character_id bigint NOT NULL,
        world_id integer NOT NULL,
        corpse_fingerprint text NOT NULL,
        granted_items jsonb NOT NULL,
        claim_kind text DEFAULT 'loot'::text NOT NULL,
        mutation_fingerprint text,
        terminal boolean DEFAULT true NOT NULL,
        armed_deadline_epoch_ticks bigint,
        shared_source boolean DEFAULT false NOT NULL,
        CONSTRAINT emulator_loot_claim_receipts_account_id_check CHECK ((account_id > 0)),
        CONSTRAINT emulator_loot_claim_receipts_armed_deadline_epoch_ticks_check CHECK (((armed_deadline_epoch_ticks IS NULL) OR (armed_deadline_epoch_ticks > 0))),
        CONSTRAINT emulator_loot_claim_receipts_character_id_check CHECK ((character_id > 0)),
        CONSTRAINT emulator_loot_claim_receipts_claim_kind_check CHECK ((claim_kind = ANY (ARRAY['loot'::text, 'gathering'::text]))),
        CONSTRAINT emulator_loot_claim_receipts_corpse_fingerprint_check CHECK ((corpse_fingerprint ~ '^[0-9a-f]{64}$'::text)),
        CONSTRAINT emulator_loot_claim_receipts_granted_items_check CHECK ((jsonb_typeof(granted_items) = 'array'::text)),
        CONSTRAINT emulator_loot_claim_receipts_mutation_fingerprint_check CHECK (((mutation_fingerprint IS NULL) OR (mutation_fingerprint ~ '^[0-9a-f]{64}$'::text))),
        CONSTRAINT emulator_loot_claim_receipts_pkey PRIMARY KEY (receipt_id),
        CONSTRAINT emulator_loot_claim_receipts_receipt_id_check CHECK ((receipt_id ~ '^[0-9a-f]{32}$'::text)),
        CONSTRAINT emulator_loot_claim_receipts_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_mail (
        id bigint GENERATED ALWAYS AS IDENTITY NOT NULL,
        account_id bigint NOT NULL,
        character_id bigint,
        world_id integer NOT NULL,
        delivery_key text NOT NULL,
        type smallint NOT NULL,
        title text NOT NULL,
        text text DEFAULT ''::text NOT NULL,
        sent_epoch_ticks bigint NOT NULL,
        expiry_epoch_ticks bigint NOT NULL,
        attachment_expiry_epoch_ticks bigint NOT NULL,
        confirmed_epoch_ticks bigint DEFAULT 0 NOT NULL,
        claimed_epoch_ticks bigint DEFAULT 0 NOT NULL,
        claimed_character_id bigint DEFAULT 0 NOT NULL,
        deleted boolean DEFAULT false NOT NULL,
        attachments jsonb DEFAULT '[]'::jsonb NOT NULL,
        CONSTRAINT emulator_mail_account_id_world_id_delivery_key_key UNIQUE (account_id, world_id, delivery_key),
        CONSTRAINT emulator_mail_attachments_check CHECK (((jsonb_typeof(attachments) = 'array'::text) AND (jsonb_array_length(attachments) <= 32))),
        CONSTRAINT emulator_mail_check CHECK ((expiry_epoch_ticks > sent_epoch_ticks)),
        CONSTRAINT emulator_mail_check1 CHECK (((attachment_expiry_epoch_ticks > sent_epoch_ticks) AND (attachment_expiry_epoch_ticks <= expiry_epoch_ticks))),
        CONSTRAINT emulator_mail_check2 CHECK (((claimed_epoch_ticks = 0) = (claimed_character_id = 0))),
        CONSTRAINT emulator_mail_claimed_character_id_check CHECK ((claimed_character_id >= 0)),
        CONSTRAINT emulator_mail_claimed_epoch_ticks_check CHECK ((claimed_epoch_ticks >= 0)),
        CONSTRAINT emulator_mail_confirmed_epoch_ticks_check CHECK ((confirmed_epoch_ticks >= 0)),
        CONSTRAINT emulator_mail_delivery_key_check CHECK (((length(delivery_key) >= 1) AND (length(delivery_key) <= 128))),
        CONSTRAINT emulator_mail_pkey PRIMARY KEY (id),
        CONSTRAINT emulator_mail_sent_epoch_ticks_check CHECK ((sent_epoch_ticks > 0)),
        CONSTRAINT emulator_mail_text_check CHECK ((octet_length(text) <= 16384)),
        CONSTRAINT emulator_mail_title_check CHECK (((octet_length(title) >= 1) AND (octet_length(title) <= 256))),
        CONSTRAINT emulator_mail_type_check CHECK (((type >= 0) AND (type <= 1))),
        CONSTRAINT emulator_mail_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_market (
        id bigint GENERATED ALWAYS AS IDENTITY NOT NULL,
        world_id integer NOT NULL,
        seller_id bigint NOT NULL,
        buyer_id bigint DEFAULT 0 NOT NULL,
        item jsonb NOT NULL,
        stackable boolean NOT NULL,
        quantity integer NOT NULL,
        main_type integer NOT NULL,
        sub_type integer NOT NULL,
        price bigint NOT NULL,
        deadline bigint NOT NULL,
        updated bigint NOT NULL,
        completed bigint DEFAULT 0 NOT NULL,
        state integer DEFAULT 0 NOT NULL,
        seller_claimed boolean DEFAULT false NOT NULL,
        buyer_claimed boolean DEFAULT false NOT NULL,
        registration_key text NOT NULL,
        CONSTRAINT emulator_market_check CHECK (((buyer_id >= 0) AND (buyer_id <> seller_id))),
        CONSTRAINT emulator_market_check1 CHECK ((((state = 1) AND (buyer_id > 0) AND (completed > 0)) OR ((state <> 1) AND (buyer_id = 0) AND (completed = 0) AND (NOT buyer_claimed)))),
        CONSTRAINT emulator_market_check2 CHECK (((state <> 0) OR (NOT seller_claimed))),
        CONSTRAINT emulator_market_completed_check CHECK ((completed >= 0)),
        CONSTRAINT emulator_market_deadline_check CHECK ((deadline > 0)),
        CONSTRAINT emulator_market_item_check CHECK ((jsonb_typeof(item) = 'object'::text)),
        CONSTRAINT emulator_market_main_type_check CHECK (((main_type >= 0) AND (main_type <= 2))),
        CONSTRAINT emulator_market_pkey PRIMARY KEY (id),
        CONSTRAINT emulator_market_price_check CHECK (((price >= 1) AND (price <= '9000000000000000'::bigint))),
        CONSTRAINT emulator_market_quantity_check CHECK ((quantity > 0)),
        CONSTRAINT emulator_market_registration_key_check CHECK (((length(registration_key) >= 16) AND (length(registration_key) <= 100))),
        CONSTRAINT emulator_market_seller_id_check CHECK ((seller_id > 0)),
        CONSTRAINT emulator_market_seller_id_registration_key_key UNIQUE (seller_id, registration_key),
        CONSTRAINT emulator_market_state_check CHECK (((state >= 0) AND (state <= 3))),
        CONSTRAINT emulator_market_sub_type_check CHECK (((sub_type >= 0) AND (sub_type <= 6))),
        CONSTRAINT emulator_market_updated_check CHECK ((updated > 0)),
        CONSTRAINT emulator_market_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_monster_reward_events (
        receipt_id text NOT NULL,
        world_id integer NOT NULL,
        recipients jsonb NOT NULL,
        ready boolean DEFAULT false NOT NULL,
        boss_script jsonb,
        boss_script_applied boolean DEFAULT true NOT NULL,
        boss_script_published boolean DEFAULT true NOT NULL,
        CONSTRAINT emulator_monster_reward_boss_script_state_check CHECK ((((NOT boss_script_published) OR boss_script_applied) AND ((boss_script IS NOT NULL) OR (boss_script_applied AND boss_script_published)))),
        CONSTRAINT emulator_monster_reward_events_pkey PRIMARY KEY (receipt_id),
        CONSTRAINT emulator_monster_reward_events_receipt_id_check CHECK ((receipt_id ~ '^[0-9a-f]{32}$'::text)),
        CONSTRAINT emulator_monster_reward_events_recipients_check CHECK ((jsonb_typeof(recipients) = 'array'::text)),
        CONSTRAINT emulator_monster_reward_events_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_monster_reward_limits (
        world_id integer NOT NULL,
        event_count bigint NOT NULL,
        CONSTRAINT emulator_monster_reward_limits_event_count_check CHECK (((event_count >= 0) AND (event_count <= 1000000))),
        CONSTRAINT emulator_monster_reward_limits_pkey PRIMARY KEY (world_id),
        CONSTRAINT emulator_monster_reward_limits_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_monster_reward_receipts (
        enqueue_sequence bigint GENERATED ALWAYS AS IDENTITY NOT NULL,
        character_id bigint NOT NULL,
        receipt_id text NOT NULL,
        account_id bigint NOT NULL,
        world_id integer NOT NULL,
        credited_unit_id bigint NOT NULL,
        npc_cms_id integer NOT NULL,
        map_id integer NOT NULL,
        pos_x_mm integer NOT NULL,
        pos_y_mm integer NOT NULL,
        pos_z_mm integer NOT NULL,
        yaw real NOT NULL,
        award_experience boolean NOT NULL,
        quest_attempts jsonb NOT NULL,
        allow_monster_offered_quests boolean NOT NULL,
        processed boolean DEFAULT false NOT NULL,
        CONSTRAINT emulator_monster_reward_receipts_credited_unit_id_check CHECK ((credited_unit_id > 0)),
        CONSTRAINT emulator_monster_reward_receipts_map_id_check CHECK ((map_id > 0)),
        CONSTRAINT emulator_monster_reward_receipts_npc_cms_id_check CHECK ((npc_cms_id > 0)),
        CONSTRAINT emulator_monster_reward_receipts_pkey PRIMARY KEY (character_id, receipt_id),
        CONSTRAINT emulator_monster_reward_receipts_quest_attempts_check CHECK ((jsonb_typeof(quest_attempts) = 'array'::text)),
        CONSTRAINT emulator_monster_reward_receipts_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_monster_reward_server_owner (
        singleton boolean DEFAULT true NOT NULL,
        epoch bigint NOT NULL,
        CONSTRAINT emulator_monster_reward_server_owner_epoch_check CHECK ((epoch >= 0)),
        CONSTRAINT emulator_monster_reward_server_owner_pkey PRIMARY KEY (singleton),
        CONSTRAINT emulator_monster_reward_server_owner_singleton_check CHECK (singleton)
    );

    CREATE TABLE public.emulator_news (
        id text NOT NULL,
        category text NOT NULL,
        title text NOT NULL,
        summary text NOT NULL,
        body text NOT NULL,
        art text NOT NULL,
        published_at bigint,
        updated_at bigint NOT NULL,
        CONSTRAINT emulator_news_art_check CHECK ((art = ANY (ARRAY['citadel'::text, 'voyage'::text, 'company'::text, 'discover'::text]))),
        CONSTRAINT emulator_news_body_check CHECK (((octet_length(body) >= 1) AND (octet_length(body) <= 16000))),
        CONSTRAINT emulator_news_category_check CHECK (((octet_length(category) >= 1) AND (octet_length(category) <= 40))),
        CONSTRAINT emulator_news_id_check CHECK ((id ~ '^[a-z0-9-]{1,80}$'::text)),
        CONSTRAINT emulator_news_pkey PRIMARY KEY (id),
        CONSTRAINT emulator_news_published_at_check CHECK ((published_at > 0)),
        CONSTRAINT emulator_news_summary_check CHECK (((octet_length(summary) >= 1) AND (octet_length(summary) <= 400))),
        CONSTRAINT emulator_news_title_check CHECK (((octet_length(title) >= 1) AND (octet_length(title) <= 160))),
        CONSTRAINT emulator_news_updated_at_check CHECK ((updated_at > 0))
    );

    CREATE TABLE public.emulator_offline_admin_quarantine (
        character_id bigint NOT NULL,
        token text NOT NULL,
        CONSTRAINT emulator_offline_admin_quarantine_character_id_check CHECK ((character_id > 0)),
        CONSTRAINT emulator_offline_admin_quarantine_pkey PRIMARY KEY (character_id),
        CONSTRAINT emulator_offline_admin_quarantine_token_check CHECK ((token ~ '^[0-9a-f]{32}$'::text))
    );

    CREATE TABLE public.emulator_quest_activity_remainders (
        world_id integer NOT NULL,
        character_id bigint NOT NULL,
        component smallint NOT NULL,
        membership_generation bigint NOT NULL,
        conversion_rate bigint NOT NULL,
        remainder bigint NOT NULL,
        CONSTRAINT emulator_quest_activity_remainders_character_id_check CHECK ((character_id > 0)),
        CONSTRAINT emulator_quest_activity_remainders_check CHECK (((remainder >= 0) AND (remainder < conversion_rate))),
        CONSTRAINT emulator_quest_activity_remainders_component_check CHECK ((component = ANY (ARRAY[0, 1]))),
        CONSTRAINT emulator_quest_activity_remainders_conversion_rate_check CHECK ((conversion_rate > 0)),
        CONSTRAINT emulator_quest_activity_remainders_membership_generation_check CHECK ((membership_generation >= 0)),
        CONSTRAINT emulator_quest_activity_remainders_pkey PRIMARY KEY (world_id, character_id, component, membership_generation, conversion_rate),
        CONSTRAINT emulator_quest_activity_remainders_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_quest_service_character_guards (
        character_id bigint NOT NULL,
        company_generation bigint DEFAULT 0 NOT NULL,
        guild_generation bigint DEFAULT 0 NOT NULL,
        CONSTRAINT emulator_quest_service_character_guard_company_generation_check CHECK ((company_generation >= 0)),
        CONSTRAINT emulator_quest_service_character_guards_character_id_check CHECK ((character_id > 0)),
        CONSTRAINT emulator_quest_service_character_guards_guild_generation_check CHECK ((guild_generation >= 0)),
        CONSTRAINT emulator_quest_service_character_guards_pkey PRIMARY KEY (character_id)
    );

    CREATE TABLE public.emulator_quest_service_effect_limits (
        world_id integer NOT NULL,
        event_count bigint NOT NULL,
        CONSTRAINT emulator_quest_service_effect_limits_event_count_check CHECK (((event_count >= 0) AND (event_count <= 1000000))),
        CONSTRAINT emulator_quest_service_effect_limits_pkey PRIMARY KEY (world_id),
        CONSTRAINT emulator_quest_service_effect_limits_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_quest_service_effects (
        enqueue_sequence bigint GENERATED ALWAYS AS IDENTITY NOT NULL,
        character_id bigint NOT NULL,
        account_id bigint NOT NULL,
        world_id integer NOT NULL,
        effect_id text NOT NULL,
        mutation_fingerprint text NOT NULL,
        monster_receipt boolean NOT NULL,
        effect_payload jsonb NOT NULL,
        expected_company_type integer NOT NULL,
        expected_guild_id bigint NOT NULL,
        expected_company_generation bigint NOT NULL,
        expected_guild_generation bigint NOT NULL,
        granted_items jsonb NOT NULL,
        company_applied boolean NOT NULL,
        guild_applied boolean NOT NULL,
        created_at timestamp with time zone DEFAULT now() NOT NULL,
        CONSTRAINT emulator_quest_service_effect_expected_company_generation_check CHECK ((expected_company_generation >= 0)),
        CONSTRAINT emulator_quest_service_effects_account_id_check CHECK ((account_id > 0)),
        CONSTRAINT emulator_quest_service_effects_character_id_check CHECK ((character_id > 0)),
        CONSTRAINT emulator_quest_service_effects_effect_id_check CHECK ((effect_id ~ '^[0-9a-f]{32}$'::text)),
        CONSTRAINT emulator_quest_service_effects_effect_payload_check CHECK ((jsonb_typeof(effect_payload) = 'object'::text)),
        CONSTRAINT emulator_quest_service_effects_expected_company_type_check CHECK (((expected_company_type >= 0) AND (expected_company_type <= 5))),
        CONSTRAINT emulator_quest_service_effects_expected_guild_generation_check CHECK ((expected_guild_generation >= 0)),
        CONSTRAINT emulator_quest_service_effects_expected_guild_id_check CHECK ((expected_guild_id >= 0)),
        CONSTRAINT emulator_quest_service_effects_granted_items_check CHECK ((jsonb_typeof(granted_items) = 'array'::text)),
        CONSTRAINT emulator_quest_service_effects_mutation_fingerprint_check CHECK ((mutation_fingerprint ~ '^[0-9a-f]{64}$'::text)),
        CONSTRAINT emulator_quest_service_effects_pkey PRIMARY KEY (character_id, effect_id),
        CONSTRAINT emulator_quest_service_effects_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_quest_service_guild_guards (
        guild_id bigint NOT NULL,
        CONSTRAINT emulator_quest_service_guild_guards_guild_id_check CHECK ((guild_id > 0)),
        CONSTRAINT emulator_quest_service_guild_guards_pkey PRIMARY KEY (guild_id)
    );

    CREATE TABLE public.emulator_ranking_records (
        world_id integer NOT NULL,
        ranking_cms_id integer NOT NULL,
        period_start bigint NOT NULL,
        holder_key text NOT NULL,
        score bigint NOT NULL,
        created bigint NOT NULL,
        member_ids bigint[] NOT NULL,
        members jsonb NOT NULL,
        CONSTRAINT emulator_ranking_records_created_check CHECK ((created > 0)),
        CONSTRAINT emulator_ranking_records_holder_key_check CHECK (((octet_length(holder_key) >= 1) AND (octet_length(holder_key) <= 200))),
        CONSTRAINT emulator_ranking_records_member_ids_check CHECK (((cardinality(member_ids) >= 1) AND (cardinality(member_ids) <= 12))),
        CONSTRAINT emulator_ranking_records_members_check CHECK (((jsonb_typeof(members) = 'array'::text) AND ((jsonb_array_length(members) >= 1) AND (jsonb_array_length(members) <= 12)))),
        CONSTRAINT emulator_ranking_records_period_start_check CHECK ((period_start > 0)),
        CONSTRAINT emulator_ranking_records_pkey PRIMARY KEY (world_id, ranking_cms_id, period_start, holder_key),
        CONSTRAINT emulator_ranking_records_ranking_cms_id_check CHECK ((ranking_cms_id > 0)),
        CONSTRAINT emulator_ranking_records_score_check CHECK ((score >= 0)),
        CONSTRAINT emulator_ranking_records_world_id_check CHECK ((world_id > 0))
    );

    CREATE TABLE public.emulator_reconnect_credentials (
        account_id bigint NOT NULL,
        credential_hash text NOT NULL,
        expires_at bigint NOT NULL,
        CONSTRAINT emulator_reconnect_credentials_credential_hash_check CHECK ((credential_hash ~ '^[0-9a-f]{64}$'::text)),
        CONSTRAINT emulator_reconnect_credentials_credential_hash_key UNIQUE (credential_hash),
        CONSTRAINT emulator_reconnect_credentials_expires_at_check CHECK ((expires_at > 0)),
        CONSTRAINT emulator_reconnect_credentials_pkey PRIMARY KEY (account_id)
    );

    CREATE TABLE public.emulator_schema_version (
        version integer NOT NULL,
        applied_at timestamp with time zone DEFAULT now() NOT NULL,
        CONSTRAINT emulator_schema_version_pkey PRIMARY KEY (version)
    );

    -- Foreign keys follow the tables so creation order does not change ownership rules.

    ALTER TABLE public.emulator_character_achievements
        ADD CONSTRAINT emulator_character_achievements_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_adventure_objects
        ADD CONSTRAINT emulator_character_adventure_objects_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_company
        ADD CONSTRAINT emulator_character_company_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_crafts
        ADD CONSTRAINT emulator_character_crafts_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_doodad_phases
        ADD CONSTRAINT emulator_character_doodad_phases_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_emotions
        ADD CONSTRAINT emulator_character_emotions_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_fog_areas
        ADD CONSTRAINT emulator_character_fog_areas_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_guild_history
        ADD CONSTRAINT emulator_character_guild_history_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_hotkeys
        ADD CONSTRAINT emulator_character_hotkeys_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_housing_items
        ADD CONSTRAINT emulator_character_housing_items_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_items
        ADD CONSTRAINT emulator_character_items_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_job_boards
        ADD CONSTRAINT emulator_character_job_boards_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_loot_first_kills
        ADD CONSTRAINT emulator_character_loot_first_kills_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_mates
        ADD CONSTRAINT emulator_character_mates_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_perks
        ADD CONSTRAINT emulator_character_perks_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_play_records
        ADD CONSTRAINT emulator_character_play_records_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_quests
        ADD CONSTRAINT emulator_character_quests_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_shop_purchases
        ADD CONSTRAINT emulator_character_shop_purchases_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_stackables
        ADD CONSTRAINT emulator_character_stackables_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_summons
        ADD CONSTRAINT emulator_character_summons_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_trade_runs
        ADD CONSTRAINT emulator_character_trade_runs_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_trade_vouchers
        ADD CONSTRAINT emulator_character_trade_vouchers_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_character_utility_slots
        ADD CONSTRAINT emulator_character_utility_slots_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_characters
        ADD CONSTRAINT emulator_characters_account_id_fkey FOREIGN KEY (account_id) REFERENCES emulator_accounts(id);

    ALTER TABLE public.emulator_discord_accounts
        ADD CONSTRAINT emulator_discord_accounts_account_id_fkey FOREIGN KEY (account_id) REFERENCES emulator_accounts(id);

    ALTER TABLE public.emulator_friend_presence
        ADD CONSTRAINT emulator_friend_presence_account_id_fkey FOREIGN KEY (account_id) REFERENCES emulator_accounts(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_friends
        ADD CONSTRAINT emulator_friends_friend_account_id_fkey FOREIGN KEY (friend_account_id) REFERENCES emulator_accounts(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_friends
        ADD CONSTRAINT emulator_friends_owner_account_id_fkey FOREIGN KEY (owner_account_id) REFERENCES emulator_accounts(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_guild_members
        ADD CONSTRAINT emulator_guild_members_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_guild_members
        ADD CONSTRAINT emulator_guild_members_guild_id_fkey FOREIGN KEY (guild_id) REFERENCES emulator_guilds(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_housing_characters
        ADD CONSTRAINT emulator_housing_characters_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_housing_packages
        ADD CONSTRAINT emulator_housing_packages_owner_character_id_fkey FOREIGN KEY (owner_character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_housing_sites
        ADD CONSTRAINT emulator_housing_sites_owner_account_id_fkey FOREIGN KEY (owner_account_id) REFERENCES emulator_accounts(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_housing_sites
        ADD CONSTRAINT emulator_housing_sites_owner_character_id_fkey FOREIGN KEY (owner_character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_mail
        ADD CONSTRAINT emulator_mail_account_id_fkey FOREIGN KEY (account_id) REFERENCES emulator_accounts(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_mail
        ADD CONSTRAINT emulator_mail_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_monster_reward_receipts
        ADD CONSTRAINT emulator_monster_reward_receipts_account_id_fkey FOREIGN KEY (account_id) REFERENCES emulator_accounts(id);

    ALTER TABLE public.emulator_monster_reward_receipts
        ADD CONSTRAINT emulator_monster_reward_receipts_character_id_fkey FOREIGN KEY (character_id) REFERENCES emulator_characters(id) ON DELETE CASCADE;

    ALTER TABLE public.emulator_monster_reward_receipts
        ADD CONSTRAINT emulator_monster_reward_receipts_receipt_id_fkey FOREIGN KEY (receipt_id) REFERENCES emulator_monster_reward_events(receipt_id);

    ALTER TABLE public.emulator_reconnect_credentials
        ADD CONSTRAINT emulator_reconnect_credentials_account_id_fkey FOREIGN KEY (account_id) REFERENCES emulator_accounts(id) ON DELETE CASCADE;

    -- Lookup and pending-work indexes used by runtime repositories.

    CREATE INDEX emulator_agent_memories_by_agent ON public.emulator_agent_memories USING btree (agent_id, id DESC);

    CREATE UNIQUE INDEX emulator_character_equipped_slot ON public.emulator_character_items USING btree (character_id, equip_slot) WHERE (equip_slot IS NOT NULL);

    CREATE UNIQUE INDEX emulator_character_name ON public.emulator_characters USING btree (world_id, lower(name));

    CREATE INDEX emulator_friends_by_friend ON public.emulator_friends USING btree (world_id, friend_account_id);

    CREATE UNIQUE INDEX emulator_gathering_sources_pending_receipt ON public.emulator_gathering_sources USING btree (pending_receipt_id) WHERE (pending_receipt_id IS NOT NULL);

    CREATE INDEX emulator_guild_members_guild ON public.emulator_guild_members USING btree (guild_id);

    CREATE UNIQUE INDEX emulator_guild_single_master ON public.emulator_guild_members USING btree (guild_id) WHERE (member_grade = 1);

    CREATE UNIQUE INDEX emulator_guild_name ON public.emulator_guilds USING btree (world_id, lower(name));

    CREATE INDEX emulator_housing_sites_owner ON public.emulator_housing_sites USING btree (world_id, owner_character_id);

    CREATE INDEX emulator_launcher_content_feed ON public.emulator_launcher_content USING btree (kind, sort_order, updated_at DESC, id) WHERE published;

    CREATE INDEX emulator_gathering_claims_unfinished ON public.emulator_loot_claim_receipts USING btree (receipt_id) WHERE ((claim_kind = 'gathering'::text) AND (NOT terminal));

    CREATE INDEX emulator_mail_recipient_index ON public.emulator_mail USING btree (account_id, world_id, character_id, id DESC) WHERE (NOT deleted);

    CREATE INDEX emulator_market_buyer ON public.emulator_market USING btree (buyer_id, id DESC) WHERE (buyer_id > 0);

    CREATE INDEX emulator_market_public ON public.emulator_market USING btree (world_id, state, main_type, sub_type, id DESC);

    CREATE INDEX emulator_market_seller ON public.emulator_market USING btree (seller_id, id DESC);

    CREATE INDEX emulator_monster_reward_boss_script_pending ON public.emulator_monster_reward_events USING btree (world_id, receipt_id) WHERE (ready AND (NOT boss_script_applied));

    CREATE INDEX emulator_monster_reward_boss_script_unpublished ON public.emulator_monster_reward_events USING btree (world_id, receipt_id) WHERE (ready AND (NOT boss_script_published));

    CREATE INDEX emulator_monster_reward_staged_idx ON public.emulator_monster_reward_events USING btree (receipt_id) WHERE (NOT ready);

    CREATE INDEX emulator_monster_reward_pending ON public.emulator_monster_reward_receipts USING btree (character_id, enqueue_sequence) WHERE (NOT processed);

    CREATE INDEX emulator_news_feed ON public.emulator_news USING btree (published_at DESC, updated_at DESC, id DESC) WHERE (published_at IS NOT NULL);

    CREATE INDEX emulator_quest_service_effects_pending_character ON public.emulator_quest_service_effects USING btree (character_id, enqueue_sequence) WHERE ((NOT company_applied) OR (NOT guild_applied));

    CREATE INDEX emulator_quest_service_effects_pending_guild ON public.emulator_quest_service_effects USING btree (expected_guild_id, enqueue_sequence) WHERE (NOT guild_applied);

    CREATE INDEX emulator_ranking_records_order ON public.emulator_ranking_records USING btree (world_id, ranking_cms_id, period_start, score, created, holder_key);

    -- A new server starts with an unclaimed reward-processing epoch.
    INSERT INTO public.emulator_monster_reward_server_owner(singleton, epoch)
        VALUES (true, 0);

    -- Compatibility markers for runtime health checks; no old migration is executed.
    INSERT INTO public.emulator_schema_version(version)
        SELECT generate_series(1, 44);
END
$chronicles_install$;
