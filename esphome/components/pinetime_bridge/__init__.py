import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, time as time_ns
from esphome.const import CONF_ID

DEPENDENCIES = ["ble_client", "time"]
AUTO_LOAD = ["ble_client"]

CONF_BLE_CLIENT_ID = "ble_client_id"
CONF_API_BASE_URL = "api_base_url"
CONF_USER_ID = "user_id"
CONF_BRIDGE_ID = "bridge_id"
CONF_POLL_INTERVAL = "poll_interval"

pinetime_bridge_ns = cg.esphome_ns.namespace("pinetime_bridge")
PineTimeBridge = pinetime_bridge_ns.class_(
    "PineTimeBridge",
    cg.Component,
    ble_client.BLEClientNode,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PineTimeBridge),
            cv.Required(CONF_BLE_CLIENT_ID): cv.use_id(ble_client.BLEClient),
            cv.Required(CONF_API_BASE_URL): cv.string,
            cv.Required(CONF_USER_ID): cv.string,
            cv.Required(CONF_BRIDGE_ID): cv.string,
            cv.Optional(CONF_POLL_INTERVAL, default="60s"): cv.positive_time_period_milliseconds,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add(var.set_api_base_url(config[CONF_API_BASE_URL]))
    cg.add(var.set_user_id(config[CONF_USER_ID]))
    cg.add(var.set_bridge_id(config[CONF_BRIDGE_ID]))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
