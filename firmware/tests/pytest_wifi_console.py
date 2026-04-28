import os
import re

import pytest
from pytest_embedded_idf.dut import IdfDut


WIFI_TEST_SSID = os.getenv("WIFI_TEST_SSID", "pytest")
WIFI_TEST_PASSWORD = os.getenv("WIFI_TEST_PASSWORD", "pytest123")
WIFI_TEST_WRONG_PASSWORD = os.getenv("WIFI_TEST_WRONG_PASSWORD", "senha_errada")


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
            re.compile(rf"Wi-Fi connected successfully|Falha ao conectar em '{re.escape(ssid)}'"),
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
            raise AssertionError(f"Wi-Fi connection to '{ssid}' did not succeed after {retries + 1} attempts")

        dut.write("1\n")


def expect_connect_failure_menu(dut: IdfDut, ssid: str) -> None:
    dut.expect_exact("Wi-Fi connection timed out or failed", timeout=60)
    dut.expect_exact(f"Falha ao conectar em '{ssid}'", timeout=5)
    dut.expect_exact("1. Tentar novamente")
    dut.expect_exact("2. Digitar outra senha")
    dut.expect_exact("3. Selecionar outra rede")
    dut.expect_exact("0. Voltar ao menu")
    dut.expect_exact("Opcao: ")


@pytest.mark.esp32
@pytest.mark.generic
def test_wifi_console_happy_path(dut: IdfDut) -> None:
    dut.expect_exact("Console Wi-Fi pronto. Pressione Enter para abrir o menu.", timeout=30)
    return_to_idle_prompt(dut)

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
def test_wifi_console_recovery_paths(dut: IdfDut) -> None:
    dut.expect_exact("Console Wi-Fi pronto. Pressione Enter para abrir o menu.", timeout=30)
    return_to_idle_prompt(dut)

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
