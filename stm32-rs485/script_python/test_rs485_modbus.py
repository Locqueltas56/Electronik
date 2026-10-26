"""
Fiches de test RS485 (maitre / esclave).

Comment ca marche :
  1. Vous tapez le nom d'une fiche de test, par exemple : Test_400
  2. Le script met les sorties de la fiche (cle "io") a l'etat voulu.
  3. Il envoie les trames de la fiche au maitre, UNE PAR UNE : le maitre
     les envoie a l'esclave et renvoie la reponse (29 octets).
  4. Chaque reponse est decodee et ecrite dans un PV de test (fichier .txt).

Autres commandes possibles au clavier :
  io S1A 1   -> met la sortie S1A a l'etat haut  (io S1A 0 : etat bas)
  in         -> lit l'etat des 7 entrees du maitre
  q          -> quitter

Pre-requis : pip install pyserial
"""

import re
import serial
from datetime import datetime


# =====================================================================
# 1. REGLAGES
# =====================================================================
PORT = "COM5"       # port serie du maitre (ST-Link) : a adapter
BAUD = 115200

# Nom de chaque sortie, tel que vous le tapez  ->  nom du signal cote STM32.
# Le script envoie <signal>_H (etat 1) ou <signal>_L (etat 0).
# Exemple : "io S1A 1" envoie SIG_A_H au maitre.
# Les noms de droite doivent etre identiques a ceux du code STM32.
IO_NOMS = {
    "S1A": "SIG_A",
    "S2A": "SIG_B",
    "S3A": "SIG_C",
    "S4A": "SIG_D",
    "S5A": "SIG_E",
    "S6A": "SIG_F",
}


# Les 7 signaux a detecter sur les entrees du maitre, dans l'ordre de la
# table ENTREES du code STM32 (le 1er nom = la 1re broche de la table).
# A remplacer par vos vrais noms de signaux.
IN_NOMS = ["E1", "E2", "E3", "E4", "E5", "E6", "E7"]

# Dernieres entrees lues pendant une fiche : {"avant": {...}, "apres": {...}}
ENTREES_FICHE = {"avant": None, "apres": None}

# Etat des sorties commandees depuis le lancement du script (rempli par
# envoyer_io). Sert a ecrire les sorties actives dans le PV.
ETAT_IO = {}


# =====================================================================
# 2. FICHES DE TEST
#    "description" : un texte libre
#    "io"          : (facultatif) sorties a positionner avant le test,
#                    ex : {"S1A": 1} = S1A a l'etat haut, 0 = etat bas
#    "trames"      : les trames a envoyer, 15 octets en hexa chacune
# =====================================================================
FICHES_TEST = {
    "Test_400": {
        "description": "Test nominal - 3 trames",
        "io": {"S1A": 1, "S2A": 0},
        "trames": [
            "14 17 27 0F 00 E4 1D 00 00 00 40 00 00 45 21",
            "14 17 27 0F 00 E4 1D 00 00 00 40 00 00 45 21",
            "14 17 27 0F 00 E4 1D 00 00 00 40 00 00 45 21",
        ],
    },
    "Test_401": {
        "description": "Test - 5 trames",
        "io": {"S3A": 1},
        "trames": [
            "14 17 27 0F 00 E4 1D 00 00 00 40 00 00 45 21",
            "14 17 27 0F 00 E4 1D 00 00 00 40 00 00 45 21",
            "14 17 27 0F 00 E4 1D 00 00 00 40 00 00 45 21",
            "14 17 27 0F 00 E4 1D 00 00 00 40 00 00 45 21",
            "14 17 27 0F 00 E4 1D 00 00 00 40 00 00 45 21",
        ],
    },
}


# =====================================================================
# 3. DECODAGE DE LA REPONSE DE L'ESCLAVE (29 octets)
#    Les octets sont numerotes de 1 a 29, comme dans le PV.
# =====================================================================

# Valeur de l'octet -> signification
DICT_ADRESSE = {0x14: "adresse 1", 0x82: "adresse 2", 0x00: "NS"}
DICT_FONCTION = {0x17: "MODBUS_RW", 0x97: "MODBUS_ERR"}
DICT_CATEGORIE = {0xE4: "categorie 1", 0xD8: "categorie 2", 0xD7: "categorie 3"}
DICT_INTER = {0x00: "defaut"}


def octet(trame, numero):
    """Donne l'octet numero 'numero' (1 = premier octet de la trame)."""
    return trame[numero - 1]


def decrire(dictionnaire, valeur):
    """Ex : decrire(DICT_ADRESSE, 0x14)  ->  '0x14 -> adresse 1'"""
    sens = dictionnaire.get(valeur, "Inconnu")
    return f"0x{valeur:02X} -> {sens}"


def temperature_tmp112(octet_fort, octet_faible):
    """Convertit les 2 octets du capteur TMP112 en degres Celsius.
    Exemple : 0x1C60 -> 28.375"""
    valeur = ((octet_fort << 8) | octet_faible) >> 4    # 12 bits utiles
    if valeur >= 2048:                                   # temperature negative
        valeur -= 4096
    return valeur * 0.0625


def analyser_trame(trame):
    """Retourne la liste des lignes qui decrivent la trame recue."""
    if len(trame) < 12:
        return [f"Trame trop courte ({len(trame)} octets), analyse impossible"]

    lignes = []

    lignes.append(f"Octet 1 (Adresse) : {decrire(DICT_ADRESSE, octet(trame, 1))}")
    lignes.append(f"Octet 2 (Fonction) : {decrire(DICT_FONCTION, octet(trame, 2))}")
    lignes.append(f"Octet 3 : 0x{octet(trame, 3):02X}")
    lignes.append(f"Octet 4 : 0x{octet(trame, 4):02X}")
    lignes.append(f"Octet 5 : 0x{octet(trame, 5):02X}")
    lignes.append(f"Octet 6 (Categorie) : {decrire(DICT_CATEGORIE, octet(trame, 6))}")

    # Octets 7 et 8 : une valeur sur 16 bits.
    # Le dernier bit (bit 0 de l'octet 8) donne le mode : 0 = MONITORING, 1 = INFO_PBIT
    valeur_16_bits = (octet(trame, 7) << 8) | octet(trame, 8)
    mode = "INFO_PBIT" if octet(trame, 8) & 1 else "MONITORING"
    lignes.append(f"Octets 7-8 (16 bits) : 0x{valeur_16_bits:04X} -> {mode}")

    # Octet 8 vu seul : le champ "Inter" (0 par defaut)
    lignes.append(f"Octet 8 (Inter) : {decrire(DICT_INTER, octet(trame, 8))}")

    # Octets 11 et 12 : temperature du capteur TMP112
    degres = temperature_tmp112(octet(trame, 11), octet(trame, 12))
    lignes.append(
        f"Octets 11-12 (TMP112) : 0x{octet(trame, 11):02X}{octet(trame, 12):02X}"
        f" -> {degres:.3f} C"
    )

    return lignes


# =====================================================================
# 4. DIALOGUE AVEC LE MAITRE
# =====================================================================
def envoyer_io(ser, nom, etat):
    """Met la sortie 'nom' (ex: 'S1A') a l'etat 0 ou 1.
    Envoie au maitre la commande <signal>_H ou <signal>_L (ex: SIG_A_H)."""
    if nom.upper() not in IO_NOMS:
        print(f"Sortie inconnue : {nom}. Noms valides : {', '.join(IO_NOMS)}")
        return
    if str(etat) not in ("0", "1"):
        print("L'etat doit etre 0 ou 1.")
        return

    niveau = "H" if str(etat) == "1" else "L"
    commande = f"{IO_NOMS[nom.upper()]}_{niveau}"
    ser.write((commande + "\r\n").encode())

    # Le maitre repond (ex: "SIG_A = 1") : on la lit pour ne pas la
    # confondre avec la reponse d'une trame plus tard.
    reponse = ser.readline().decode(errors="ignore").strip()
    print(f"{nom.upper()} = {etat}  ({commande})  ->  {reponse or 'pas de reponse du maitre'}")

    # On ne memorise l'etat que si le maitre a bien repondu et reconnu la commande
    if reponse and "inconnue" not in reponse.lower():
        ETAT_IO[nom.upper()] = int(etat)


def lire_entrees(ser):
    """Demande au maitre l'etat des 7 entrees. Le maitre repond une ligne
    'IN:0101101' (un chiffre par entree). Retourne {nom: 0 ou 1}, ou None."""
    ser.write(b"READ_IN\r\n")
    ligne = ser.readline().decode(errors="ignore").strip()

    if ligne.startswith("IN:") and len(ligne) == 3 + len(IN_NOMS):
        return {nom: int(c) for nom, c in zip(IN_NOMS, ligne[3:])}

    print(f"Lecture des entrees impossible (reponse du maitre : {ligne or 'aucune'})")
    return None


def afficher_entrees(entrees):
    texte = "  ".join(f"{nom}={etat}" for nom, etat in entrees.items())
    print(f"Entrees : {texte}")


def lire_reponse_trame(ser):
    """Attend la reponse du maitre a une trame : une ligne qui commence par
    'Trame'. Retourne cette ligne, ou '' si rien n'arrive."""
    while True:
        ligne = ser.readline().decode(errors="ignore").strip()
        if ligne:
            print(ligne)
        if ligne == "" or ligne.startswith("Trame"):
            return ligne


def octets_de_la_ligne(ligne):
    """'Trame 01 : 14 17 27 ..'  ->  les octets 14 17 27 .. """
    partie_hexa = ligne.split(":", 1)[1]
    return bytes(int(x, 16) for x in partie_hexa.split())


def executer_fiche(ser, fiche):
    """Execute une fiche. Retourne la liste des resultats, un par trame :
    {"numero": 1, "envoyee": <trame envoyee>, "octets": <reponse>, "erreur": None}
    {"numero": 1, "envoyee": <trame envoyee>, "octets": None, "erreur": "<message>"}"""
    resultats = []

    # 1. Positionner les sorties de la fiche
    for nom, etat in fiche.get("io", {}).items():
        envoyer_io(ser, nom, etat)

    # 1 bis. Etat des entrees avant d'envoyer les trames
    ENTREES_FICHE["avant"] = lire_entrees(ser)

    # 2. Dire au maitre combien de trames vont arriver
    trames = fiche["trames"]
    ser.write(f"SEQ:{len(trames)}\r\n".encode())

    # 3. Envoyer les trames une par une, en attendant la reponse a chaque fois
    for numero, trame in enumerate(trames, start=1):
        ser.write((trame + "\r\n").encode())
        ligne = lire_reponse_trame(ser)

        if ligne == "":
            erreur = "pas de reponse du maitre"
        elif "TIMEOUT" in ligne or "invalide" in ligne:
            erreur = ligne
        else:
            erreur = None

        if erreur:
            resultats.append({"numero": numero, "envoyee": trame, "octets": None, "erreur": erreur})
            break       # le maitre s'arrete a la premiere erreur : on s'arrete aussi
        resultats.append({"numero": numero, "envoyee": trame, "octets": octets_de_la_ligne(ligne), "erreur": None})

    # 4. Le maitre termine par "Sequence terminee."
    fin = ser.readline().decode(errors="ignore").strip()
    if fin:
        print(fin)

    # 5. Etat des entrees apres les trames
    ENTREES_FICHE["apres"] = lire_entrees(ser)

    return resultats


# =====================================================================
# 5. PV DE TEST (fichier .txt)
# =====================================================================
def ecrire_pv(resultats, nom_fiche, description):
    maintenant = datetime.now()
    nom_fichier = f"PV_{nom_fiche}_{maintenant.strftime('%Y%m%d_%H%M%S')}.txt"

    # --- En-tete ---
    lignes = [
        "=" * 70,
        "PV DE TEST - LIAISON RS485 MAITRE/ESCLAVE",
        "=" * 70,
        f"Date du test     : {maintenant.strftime('%d/%m/%Y %H:%M:%S')}",
        f"Fiche de test    : {nom_fiche} - {description}",
        f"Port serie       : {PORT} @ {BAUD} bauds",
        f"Nombre de trames : {len(resultats)}",
        "",
        "Etat des sorties pendant le test :",
    ]
    for nom_io in IO_NOMS:
        if nom_io in ETAT_IO:
            etat = "1 (actif)" if ETAT_IO[nom_io] == 1 else "0"
        else:
            etat = "non commande"
        lignes.append(f"  {nom_io} ({IO_NOMS[nom_io]}) : {etat}")
    lignes.append("")

    avant, apres = ENTREES_FICHE["avant"], ENTREES_FICHE["apres"]
    if avant or apres:
        lignes.append("Entrees detectees (avant -> apres les trames) :")
        for nom_in in IN_NOMS:
            a = avant[nom_in] if avant else "?"
            b = apres[nom_in] if apres else "?"
            lignes.append(f"  {nom_in} : {a} -> {b}")
        lignes.append("")

    # --- Une section par trame ---
    nb_ok = 0
    for r in resultats:
        lignes.append(f"--- Trame {r['numero']:02d} ---")
        lignes.append("Trame envoyee (hex) : " + " ".join(r["envoyee"].upper().split()))

        if r["erreur"]:
            lignes.append(f"Resultat : ECHEC ({r['erreur']})")
            lignes.append("")
            continue

        nb_ok += 1
        octets = r["octets"]
        lignes.append("Trame recue (hex)   : " + " ".join(f"{b:02X}" for b in octets))

        lignes.append("Detail par octet :")
        for numero, valeur in enumerate(octets, start=1):
            lignes.append(f"  Octet {numero:02d} : 0x{valeur:02X}")

        lignes.append("Analyse :")
        for texte in analyser_trame(octets):
            lignes.append(f"  {texte}")
        lignes.append("")

    # --- Synthese ---
    lignes += [
        "-" * 70,
        "SYNTHESE",
        "-" * 70,
        f"Trames recues : {nb_ok} / {len(resultats)}",
        f"Echecs        : {len(resultats) - nb_ok} / {len(resultats)}",
    ]

    with open(nom_fichier, "w", encoding="utf-8") as fichier:
        fichier.write("\n".join(lignes))
    print(f"\nPV de test genere : {nom_fichier}")


# =====================================================================
# 6. PROGRAMME PRINCIPAL
# =====================================================================
def trouver_fiche(saisie):
    """Retrouve le nom exact d'une fiche a partir de ce qui est tape, sans
    tenir compte des majuscules, ni des espaces / tirets / underscores /
    caracteres invisibles.
    Ex : 'test 400', 'TEST-400' et 'test_400' trouvent 'Test_400'.
    Retourne None si aucune fiche ne correspond."""
    def simplifier(texte):
        return re.sub(r"[^a-z0-9]", "", texte.lower())

    for nom in FICHES_TEST:
        if simplifier(nom) == simplifier(saisie):
            return nom
    return None


if __name__ == "__main__":
    with serial.Serial(PORT, BAUD, timeout=1) as ser:
        while True:
            saisie = input("Fiche de test, 'io <nom> <0|1>', 'in' ou 'q' : ").strip()
            mots = saisie.split()

            if saisie.lower() == "q":
                break

            elif saisie.lower() == "in":
                entrees = lire_entrees(ser)
                if entrees:
                    afficher_entrees(entrees)

            elif len(mots) == 3 and mots[0].lower() == "io":
                envoyer_io(ser, mots[1], mots[2])

            elif trouver_fiche(saisie):
                nom_fiche = trouver_fiche(saisie)
                fiche = FICHES_TEST[nom_fiche]
                resultats = executer_fiche(ser, fiche)
                if resultats:
                    ecrire_pv(resultats, nom_fiche, fiche["description"])
                else:
                    print("Aucune trame recue.")

            else:
                print(f"Commande inconnue : {ascii(saisie)}")
                print("Fiches disponibles :", ", ".join(ascii(nom) for nom in FICHES_TEST))
