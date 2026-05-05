import os
import re
import subprocess

import pytest
from pytest_embedded_idf.dut import IdfDut


WIFI_TEST_SSID = os.getenv("WIFI_TEST_SSID", "pytest")
WIFI_TEST_PASSWORD = os.getenv("WIFI_TEST_PASSWORD", "pytest123")
WIFI_TEST_WRONG_PASSWORD = os.getenv("WIFI_TEST_WRONG_PASSWORD", "senha_errada")
WIFI_AP_MANUAL = os.getenv("WIFI_AP_MANUAL", "").lower() in {"1", "true", "yes", "on"}

# Comandos externos para desligar/ligar o AP durante o teste.
# Exemplos:
# WIFI_AP_DOWN_CMD="./scripts/ap_down.sh"
# WIFI_AP_UP_CMD="./scripts/ap_up.sh"
WIFI_AP_DOWN_CMD = os.getenv("WIFI_AP_DOWN_CMD")
WIFI_AP_UP_CMD = os.getenv("WIFI_AP_UP_CMD")


def run_required_cmd(cmd: str | None, name: str) -> None:
    if not cmd:
        pytest.skip(f"{name} não configurado")

    subprocess.run(cmd, shell=True, check=True)


def run_optional_cmd(cmd: str | None) -> None:
    if cmd:
        subprocess.run(cmd, shell=True, check=False)


def _manual_ap_step(action: str, ssid: str) -> None:
    print()
    print(f"[manual] {action} a rede/AP '{ssid}' e pressione Enter para continuar...")
    input()


def bring_ap_down(ssid: str) -> bool:
    if WIFI_AP_DOWN_CMD:
        run_required_cmd(WIFI_AP_DOWN_CMD, "WIFI_AP_DOWN_CMD")
        return True

    if WIFI_AP_MANUAL:
        _manual_ap_step("Desligue", ssid)
        return True

    return False


def bring_ap_up(ssid: str) -> bool:
    if WIFI_AP_UP_CMD:
        run_required_cmd(WIFI_AP_UP_CMD, "WIFI_AP_UP_CMD")
        return True

    if WIFI_AP_MANUAL:
        _manual_ap_step("Ligue", ssid)
        return True

    return False


def expect_menu(dut: IdfDut) -> None:
    dut.expect_exact("Menu Wi-Fi")
    dut.expect_exact("1. Conectar/configurar Wi-Fi")
    dut.expect_exact("2. Limpar credenciais salvas")
    dut.expect_exact("3. Mostrar status")
    dut.expect_exact("0. Voltar")
    dut.expect_exact("Opcao: ")


def open_menu(dut: IdfDut) -> None:
    dut.write("\n")
    expect_menu(dut)


def return_to_idle_prompt(dut: IdfDut) -> None:
    dut.expect_exact("[Enter] menu Wi-Fi > ")


def clear_saved_credentials_from_menu(dut: IdfDut) -> None:
    dut.write("2\n")
    dut.expect("Wi-Fi credentials erased from NVS|Failed to erase Wi-Fi credentials")
    expect_menu(dut)


def clear_saved_credentials(dut: IdfDut) -> None:
    open_menu(dut)
    clear_saved_credentials_from_menu(dut)
    dut.write("0\n")
    return_to_idle_prompt(dut)


def start_manual_wifi_flow(dut: IdfDut, ssid: str, password: str) -> None:
    start_manual_wifi_flow_from_menu(dut, ssid, password)


def start_manual_wifi_flow_from_menu(dut: IdfDut, ssid: str, password: str) -> None:
    dut.write("1\n")
    dut.expect(re.compile(r"Escolha \[1-5/n/p/r/m\]: "), timeout=60)
    dut.write("m\n")
    dut.expect_exact("SSID manual: ")
    dut.write(f"{ssid}\n")
    dut.expect_exact("Senha: ")
    dut.write(f"{password}\n")


def expect_connect_success(dut: IdfDut, ssid: str) -> None:
    expect_connect_success_with_retry(dut, ssid, retries=0)


def expect_connect_success_with_retry(dut: IdfDut, ssid: str, retries: int = 2) -> None:
    for attempt in range(retries + 1):
        dut.expect_exact(f"Connecting to SSID '{ssid}'", timeout=60)

        result = dut.expect(
            re.compile(
                rf"Wi-Fi connected successfully|Falha ao conectar em '{re.escape(ssid)}'"
            ),
            timeout=60,
        )

        match_text = result.group(0)
        if isinstance(match_text, bytes):
            match_text = match_text.decode(errors="ignore")

        if match_text == "Wi-Fi connected successfully":
            expect_menu(dut)
            return

        dut.expect_exact("1. Tentar novamente")
        dut.expect_exact("2. Digitar outra senha")
        dut.expect_exact("3. Selecionar outra rede")
        dut.expect_exact("0. Voltar ao menu")
        dut.expect_exact("Opcao: ")

        if attempt == retries:
            raise AssertionError(
                f"Wi-Fi connection to '{ssid}' did not succeed after {retries + 1} attempts"
            )

        dut.write("1\n")


def expect_connect_failure_menu(dut: IdfDut, ssid: str) -> None:
    dut.expect_exact("Wi-Fi connection timed out or failed", timeout=60)
    dut.expect_exact(f"Falha ao conectar em '{ssid}'", timeout=5)
    dut.expect_exact("1. Tentar novamente")
    dut.expect_exact("2. Digitar outra senha")
    dut.expect_exact("3. Selecionar outra rede")
    dut.expect_exact("0. Voltar ao menu")
    dut.expect_exact("Opcao: ")


def expect_auto_connect_success(dut: IdfDut, ssid: str) -> None:
    dut.expect_exact(f"Auto-connecting to saved Wi-Fi SSID '{ssid}'", timeout=30)
    dut.expect_exact(f"Connecting to SSID '{ssid}'", timeout=10)
    dut.expect_exact("Wi-Fi connected successfully", timeout=120)


def expect_auto_connect_attempt(dut: IdfDut, ssid: str, timeout: int = 60) -> None:
    dut.expect_exact(f"Auto-connecting to saved Wi-Fi SSID '{ssid}'", timeout=timeout)
    dut.expect_exact(f"Connecting to SSID '{ssid}'", timeout=10)


def expect_boot_ready(dut: IdfDut) -> None:
    dut.expect_exact("Console Wi-Fi pronto. Pressione Enter para abrir o menu.", timeout=30)
    return_to_idle_prompt(dut)


@pytest.mark.esp32
@pytest.mark.generic
def test_wifi_console_happy_path(dut: IdfDut) -> None:
    expect_boot_ready(dut)

    clear_saved_credentials(dut)

    start_manual_wifi_flow(dut, WIFI_TEST_SSID, WIFI_TEST_PASSWORD)
    expect_connect_success_with_retry(dut, WIFI_TEST_SSID)

    dut.write("3\n")
    dut.expect(re.compile(r"Wi-Fi conectado em '.*' \| RSSI -?\d+"), timeout=10)
    expect_menu(dut)

    dut.write("0\n")
    return_to_idle_prompt(dut)


@pytest.mark.esp32
@pytest.mark.generic
def test_wifi_auto_connects_with_saved_credentials_on_boot(dut: IdfDut) -> None:
    expect_boot_ready(dut)

    clear_saved_credentials(dut)

    start_manual_wifi_flow(dut, WIFI_TEST_SSID, WIFI_TEST_PASSWORD)
    expect_connect_success_with_retry(dut, WIFI_TEST_SSID)

    dut.write("0\n")
    return_to_idle_prompt(dut)

    dut._hard_reset()

    expect_boot_ready(dut)
    expect_auto_connect_success(dut, WIFI_TEST_SSID)


@pytest.mark.esp32
@pytest.mark.generic
def test_wifi_console_enter_has_priority_over_boot_auto_connect(dut: IdfDut) -> None:
    expect_boot_ready(dut)

    clear_saved_credentials(dut)

    start_manual_wifi_flow(dut, WIFI_TEST_SSID, WIFI_TEST_PASSWORD)
    expect_connect_success_with_retry(dut, WIFI_TEST_SSID)

    dut.write("0\n")
    return_to_idle_prompt(dut)

    dut._hard_reset()

    expect_boot_ready(dut)

    open_menu(dut)
    clear_saved_credentials_from_menu(dut)

    dut.write("0\n")
    return_to_idle_prompt(dut)


@pytest.mark.esp32
@pytest.mark.generic
def test_wifi_boot_auto_connect_retries_until_network_returns(dut: IdfDut) -> None:
    expect_boot_ready(dut)

    clear_saved_credentials(dut)

    start_manual_wifi_flow(dut, WIFI_TEST_SSID, WIFI_TEST_PASSWORD)
    expect_connect_success_with_retry(dut, WIFI_TEST_SSID)

    dut.write("0\n")
    return_to_idle_prompt(dut)

    if not (WIFI_AP_DOWN_CMD and WIFI_AP_UP_CMD) and not WIFI_AP_MANUAL:
        pytest.skip("Configure WIFI_AP_DOWN_CMD/WIFI_AP_UP_CMD ou WIFI_AP_MANUAL=1")

    try:
        bring_ap_down(WIFI_TEST_SSID)

        dut._hard_reset()

        expect_boot_ready(dut)
        expect_auto_connect_attempt(dut, WIFI_TEST_SSID, timeout=60)
        dut.expect_exact("Automatic Wi-Fi connection attempt failed", timeout=30)
        expect_auto_connect_attempt(dut, WIFI_TEST_SSID, timeout=60)

        bring_ap_up(WIFI_TEST_SSID)
        dut.expect_exact("Automatic Wi-Fi connection attempt failed", timeout=30)
        expect_auto_connect_success(dut, WIFI_TEST_SSID)

    finally:
        if WIFI_AP_DOWN_CMD or WIFI_AP_UP_CMD:
            run_optional_cmd(WIFI_AP_UP_CMD)


@pytest.mark.esp32
@pytest.mark.generic
def test_wifi_console_recovery_paths(dut: IdfDut) -> None:
    expect_boot_ready(dut)

    clear_saved_credentials(dut)
    open_menu(dut)

    start_manual_wifi_flow_from_menu(dut, WIFI_TEST_SSID, WIFI_TEST_WRONG_PASSWORD)
    expect_connect_failure_menu(dut, WIFI_TEST_SSID)

    dut.write("1\n")
    expect_connect_failure_menu(dut, WIFI_TEST_SSID)

    dut.write("2\n")
    dut.expect_exact("Nova senha: ")
    dut.write(f"{WIFI_TEST_PASSWORD}\n")
    expect_connect_success_with_retry(dut, WIFI_TEST_SSID)

    clear_saved_credentials_from_menu(dut)

    start_manual_wifi_flow_from_menu(dut, WIFI_TEST_SSID, WIFI_TEST_WRONG_PASSWORD)
    expect_connect_failure_menu(dut, WIFI_TEST_SSID)

    dut.write("3\n")
    dut.expect(re.compile(r"Escolha \[1-5/n/p/r/m\]: "), timeout=60)
    dut.write("m\n")
    dut.expect_exact("SSID manual: ")
    dut.write(f"{WIFI_TEST_SSID}\n")
    dut.expect_exact("Senha: ")
    dut.write(f"{WIFI_TEST_PASSWORD}\n")
    expect_connect_success_with_retry(dut, WIFI_TEST_SSID)

    dut.write("3\n")
    dut.expect(re.compile(r"Wi-Fi conectado em '.*' \| RSSI -?\d+"), timeout=10)
    expect_menu(dut)

    dut.write("0\n")
    return_to_idle_prompt(dut)


@pytest.mark.esp32
@pytest.mark.generic
def test_wifi_restarts_when_connected_wifi_is_lost_reason_1(dut: IdfDut) -> None:
    expect_boot_ready(dut)

    clear_saved_credentials(dut)

    start_manual_wifi_flow(dut, WIFI_TEST_SSID, WIFI_TEST_PASSWORD)
    expect_connect_success_with_retry(dut, WIFI_TEST_SSID)

    dut.write("0\n")
    return_to_idle_prompt(dut)

    if not (WIFI_AP_DOWN_CMD and WIFI_AP_UP_CMD) and not WIFI_AP_MANUAL:
        pytest.skip("Configure WIFI_AP_DOWN_CMD/WIFI_AP_UP_CMD ou WIFI_AP_MANUAL=1")

    try:
        bring_ap_down(WIFI_TEST_SSID)

        dut.expect(
            re.compile(r"Disconnected from AP, reason=1"),
            timeout=90,
        )

        dut.expect(
            re.compile(
                r"Wi-Fi connection lost after IP was acquired\. Restarting ESP32, reason=1"
            ),
            timeout=20,
        )

        dut.expect_exact(
            "Console Wi-Fi pronto. Pressione Enter para abrir o menu.",
            timeout=90,
        )

    finally:
        if WIFI_AP_DOWN_CMD or WIFI_AP_UP_CMD:
            run_optional_cmd(WIFI_AP_UP_CMD)
        elif WIFI_AP_MANUAL:
            bring_ap_up(WIFI_TEST_SSID)
